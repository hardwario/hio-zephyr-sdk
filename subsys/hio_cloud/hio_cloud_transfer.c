/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_backend.h"
#include "hio_cloud_packet.h"
#include "hio_cloud_transfer.h"
#include "hio_cloud_config.h"

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_cloud.h>
#include <hio/hio_lte.h>
#include <hio/hio_info.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define MODEM_SLEEP_DELAY K_SECONDS(10)

/* Positive so it never collides with the negative errno codes propagated to
 * the cloud layer or the 0 success code. Returned by transfer() when the
 * active address was switched mid-exchange: the caller restarts the logical
 * transfer from the first fragment with a fresh sequence and never propagates
 * this value outward. */
#define TRANSFER_ADDR_SWITCHED 1

/* A failed exchange is retried by resending the very same packet with the same
 * sequence: the server treats it as a duplicate and replays the cached
 * response, so no progress is lost. Resending continues until the caller's
 * timeout expires rather than for a fixed count, which lets a slow network
 * (few connection grants) eventually get the packet through.
 *
 * The backoff before a resend depends on why the previous attempt failed:
 * a lost reply (-ETIMEDOUT) can be re-solicited promptly, while the network
 * not granting a connection (-ENOTCONN) means waiting is pointless until it
 * does, so back off longer to avoid hammering a congested cell. */
#define TRANSFER_BACKOFF_LOST_REPLY K_SECONDS(1)
#define TRANSFER_BACKOFF_NO_CONN    K_SECONDS(30)

/* How many times a stale (lagging) ACK is answered by re-sending the same
 * poll/ack before the transfer is restarted, bounding the every-other-packet
 * RAI handshake so a persistently out-of-step peer cannot spin forever. */
#define TRANSFER_REPEAT_LIMIT 3

/* Internal cap applied to a K_FOREVER wait_ready. The LTE layer sets
 * CONNECTED_BIT only after the socket opens, and with DTLS against a dead
 * server the handshake in nrf_connect never succeeds — a K_FOREVER wait
 * (init_step in hio_cloud waits before every step) would then block forever,
 * no send_recv transaction would ever be created, and the address failover
 * (counted on failed attempts) would never activate. Capping the wait lets
 * the caller proceed: init_step ignores the return value, the subsequent
 * send_recv sets a pending transaction, and the LTE ERROR path ends it with
 * -ENOTCONN so failures become countable. Finite caller timeouts are passed
 * through unchanged. */
#define WAIT_READY_FOREVER_CAP K_SECONDS(30)

LOG_MODULE_REGISTER(cloud_transfer, CONFIG_HIO_CLOUD_LOG_LEVEL);

HIO_BUF_DEFINE_STATIC(m_buf_0, HIO_LTE_UDP_MAX_MTU);
HIO_BUF_DEFINE_STATIC(m_buf_1, HIO_LTE_UDP_MAX_MTU);

static uint32_t m_serial_number;
static uint8_t m_token[16];

static uint16_t m_sequence;
static uint16_t m_last_recv_sequence;
static struct hio_cloud_packet m_pck_send;
static struct hio_cloud_packet m_pck_recv;
static struct hio_cloud_transfer_metrics m_metrics = {
	.uplink_last_ts = -1,
	.downlink_last_ts = -1,
};
static K_MUTEX_DEFINE(m_lock_metrics);

/* Failover state (reset on boot). Mutated only by failover_report_attempt() on
 * the cloud work-queue thread; read by get_failover_state() from the shell.
 * Plain 32-bit accesses are atomic on the target, so no lock is taken here (it
 * must in particular not run under m_lock_metrics). */
static int m_active_idx;
static int m_consecutive_failures;
static uint32_t m_failover_count;

/* Collect the non-empty configured addresses in order (addr, addr2, addr3);
 * empty entries are skipped, so addr + addr3 yields a two-entry list. Returns
 * the number of entries written to @p addrs. */
static int build_addr_list(const char *addrs[3])
{
	int count = 0;

	if (g_hio_cloud_config.addr[0]) {
		addrs[count++] = g_hio_cloud_config.addr;
	}
	if (g_hio_cloud_config.addr2[0]) {
		addrs[count++] = g_hio_cloud_config.addr2;
	}
	if (g_hio_cloud_config.addr3[0]) {
		addrs[count++] = g_hio_cloud_config.addr3;
	}

	return count;
}

static int fill_socket_config(struct hio_lte_socket_config *cfg, const char *addr)
{
	memset(cfg, 0, sizeof(*cfg));
	/* memset above zeroed the whole struct, so copying at most size-1 bytes
	 * leaves the last byte NUL — cfg->addr stays null-terminated. */
	strncpy(cfg->addr, addr, sizeof(cfg->addr) - 1);

	switch (g_hio_cloud_config.protocol) {
	case HIO_CLOUD_PROTOCOL_FLAP_HASH:
		cfg->dtls_enabled = false;
		cfg->port = g_hio_cloud_config.port_signed;
		break;
	case HIO_CLOUD_PROTOCOL_FLAP_DTLS:
		cfg->dtls_enabled = true;
		cfg->port = g_hio_cloud_config.port_dtls;
		break;
	default:
		LOG_ERR("Unsupported cloud protocol");
		return -EPROTONOSUPPORT;
	}

	return 0;
}

/* Called after every hio_lte_send_recv attempt in the transfer() retry loop.
 * Counts consecutive failures and, once the threshold is reached with more than
 * one address configured, rotates to the next address via
 * hio_lte_update_socket_config(). Returns true only when the active address was
 * switched, so the caller aborts and restarts the logical transfer. */
static bool failover_report_attempt(bool success)
{
	if (success) {
		m_consecutive_failures = 0;
		return false;
	}

	m_consecutive_failures++;

	int failover = g_hio_cloud_config.failover;
	if (failover <= 0) {
		return false;
	}

	const char *addrs[3];
	int count = build_addr_list(addrs);
	if (count <= 1) {
		return false;
	}

	/* The address list can shrink if the config changed since boot; keep
	 * m_active_idx in range so indexing addrs[] below stays safe. */
	if (m_active_idx >= count) {
		m_active_idx = 0;
	}

	if (m_consecutive_failures < failover) {
		return false;
	}

	int candidate_idx = (m_active_idx + 1) % count;

	struct hio_lte_socket_config socket_config;
	int ret = fill_socket_config(&socket_config, addrs[candidate_idx]);
	if (ret) {
		return false;
	}

	ret = hio_lte_update_socket_config(&socket_config);
	if (ret) {
		LOG_ERR("Call `hio_lte_update_socket_config` failed: %d", ret);
		/* State unchanged; the next failure retries the switch. */
		return false;
	}

	LOG_WRN("Failover: switching address from %s to %s after %d consecutive failures",
		addrs[m_active_idx], addrs[candidate_idx], m_consecutive_failures);

	m_active_idx = candidate_idx;
	m_failover_count++;
	m_consecutive_failures = 0;

	return true;
}

static size_t transfer_mode_max_data_size(void)
{
	size_t mtu;
	if (hio_lte_get_socket_mtu(&mtu)) {
		return 0;
	}

	switch (g_hio_cloud_config.protocol) {
	case HIO_CLOUD_PROTOCOL_FLAP_HASH:
		return (mtu - HIO_CLOUD_PACKET_SIGNED_MIN_SIZE - HIO_CLOUD_PACKET_HEADER_SIZE);
	case HIO_CLOUD_PROTOCOL_FLAP_DTLS:
		return (mtu - HIO_CLOUD_PACKET_HEADER_SIZE);
	default:
		return 0;
	}
}
static int transfer(struct hio_cloud_packet *pck_send, struct hio_cloud_packet *pck_recv, bool rai,
		    k_timeout_t timeout)
{
	int ret;

	struct hio_buf *send_buf = &m_buf_0;
	struct hio_buf *recv_buf = &m_buf_1;
	size_t len;

	LOG_INF("Sending packet Sequence: %u %s len: %u", pck_send->sequence,
		hio_cloud_packet_flags_to_str(pck_send->flags), pck_send->data_len);

	hio_buf_reset(send_buf);

	switch (g_hio_cloud_config.protocol) {
	case HIO_CLOUD_PROTOCOL_FLAP_HASH:
		ret = hio_cloud_packet_signed_pack(pck_send, m_serial_number, m_token, send_buf);
		if (ret) {
			LOG_ERR("Call `hio_cloud_packet_signed_pack` failed: %d", ret);
			return ret;
		}
		break;
	case HIO_CLOUD_PROTOCOL_FLAP_DTLS:
		ret = hio_cloud_packet_pack(pck_send, send_buf);
		if (ret) {
			LOG_ERR("Call `hio_cloud_packet_pack` failed: %d", ret);
			return ret;
		}
		break;
	default:
		return -EPROTONOSUPPORT;
	}

	LOG_HEXDUMP_INF(hio_buf_get_mem(send_buf), hio_buf_get_used(send_buf),
			rai ? "Sending packet RAI:" : "Sending packet:");

	/* The packet is serialized into send_buf once above. On a failed exchange
	 * resend the same bytes with the same sequence (the server replays its
	 * cached response, so the transfer keeps its place) until the caller's
	 * timeout expires. Resending is only meaningful when a response is
	 * expected (pck_recv set). */
	k_timepoint_t end = sys_timepoint_calc(timeout);
	for (int attempt = 0; ; attempt++) {
		len = 0;

		struct hio_lte_send_recv_param param = {
			.rai = rai,
			.send_buf = hio_buf_get_mem(send_buf),
			.send_len = hio_buf_get_used(send_buf),
			.recv_buf = NULL,
			.recv_size = 0,
			.recv_len = &len,
			.timeout = timeout,
			/* No LTE-level retry: retransmission is owned by this layer
			 * (the loop below), so the FSM returns instead of resending
			 * on its own. */
			.retry_count = 0,
		};

		if (pck_recv) {
			hio_buf_reset(recv_buf);
			param.recv_buf = hio_buf_get_mem(recv_buf);
			param.recv_size = hio_buf_get_free(recv_buf);
		}

		ret = hio_lte_send_recv(&param);

		/* Feed the failover counter with every attempt (outside the
		 * metrics lock). A switch aborts this exchange so the caller can
		 * restart the logical transfer against the new address. */
		if (failover_report_attempt(ret == 0)) {
			return TRANSFER_ADDR_SWITCHED;
		}

		if (!ret) {
			break;
		}

		/* Only an exchange that expects a reply is worth resending, and
		 * only while the caller's deadline has not passed. */
		bool retryable = pck_recv && (ret == -ETIMEDOUT || ret == -ENOTCONN);
		if (retryable && !sys_timepoint_expired(end)) {
			k_timeout_t backoff = (ret == -ENOTCONN) ? TRANSFER_BACKOFF_NO_CONN
								  : TRANSFER_BACKOFF_LOST_REPLY;
			LOG_WRN("Exchange failed (%d), resending packet after backoff (attempt %d)",
				ret, attempt + 2);
			k_sleep(backoff);
			continue;
		}

		LOG_ERR("Call `hio_lte_send_recv` failed: %d", ret);
		return ret;
	}

	if (pck_recv) {
		ret = hio_buf_seek(recv_buf, len);
		if (ret) {
			LOG_ERR("Call `hio_buf_seek` failed: %d", ret);
			return ret;
		}

		if (!hio_buf_get_used(recv_buf)) {
			LOG_ERR("No data received");
			return -EIO;
		}

		LOG_HEXDUMP_INF(hio_buf_get_mem(recv_buf), hio_buf_get_used(recv_buf),
				"Received packet:");

		len = 0;

		switch (g_hio_cloud_config.protocol) {
		case HIO_CLOUD_PROTOCOL_FLAP_HASH:

			uint32_t serial_number;
			ret = hio_cloud_packet_signed_unpack(pck_recv, &serial_number, m_token,
							     recv_buf);
			if (ret) {
				LOG_ERR("Call `hio_cloud_packet_signed_unpack` failed: %d", ret);
				return ret;
			}

			if (serial_number != m_serial_number) {
				LOG_ERR("Serial number mismatch");
				return -EREMCHG;
			}
			break;
		case HIO_CLOUD_PROTOCOL_FLAP_DTLS:
			ret = hio_cloud_packet_unpack(pck_recv, recv_buf);
			if (ret) {
				LOG_ERR("Call `hio_cloud_packet_unpack` failed: %d", ret);
				return ret;
			}
			break;
		}

		LOG_INF("Received packet Sequence: %u %s len: %u", pck_recv->sequence,
			hio_cloud_packet_flags_to_str(pck_recv->flags), pck_recv->data_len);
	}

	return 0;
}

int hio_cloud_transfer_init(uint32_t serial_number, const uint8_t token[16])
{
	memset(&m_pck_send, 0, sizeof(m_pck_send));
	memset(&m_pck_recv, 0, sizeof(m_pck_recv));

	m_serial_number = serial_number;
	memcpy(m_token, token, sizeof(m_token));

	m_sequence = 0;
	m_last_recv_sequence = 0;

	m_active_idx = 0;
	m_consecutive_failures = 0;
	m_failover_count = 0;

	hio_cloud_transfer_reset_metrics();

	const char *addrs[3];
	int count = build_addr_list(addrs);
	const char *addr = count > 0 ? addrs[0] : g_hio_cloud_config.addr;

	struct hio_lte_socket_config socket_config;
	int ret = fill_socket_config(&socket_config, addr);
	if (ret) {
		return ret;
	}

	hio_lte_enable(&socket_config);

	return 0;
}

int hio_cloud_transfer_wait_for_ready(k_timeout_t timeout)
{
	int ret;

	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		timeout = WAIT_READY_FOREVER_CAP;
	}

	ret = hio_lte_wait_for_connected(timeout);
	if (ret) {
		LOG_ERR("Call `hio_lte_wait_for_connected` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_cloud_transfer_reset_metrics(void)
{
	k_mutex_lock(&m_lock_metrics, K_FOREVER);
	memset(&m_metrics, 0, sizeof(m_metrics));
	k_mutex_unlock(&m_lock_metrics);
	return 0;
}

int hio_cloud_transfer_get_metrics(struct hio_cloud_transfer_metrics *metrics)
{
	if (!metrics) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock_metrics, K_FOREVER);
	memcpy(metrics, &m_metrics, sizeof(m_metrics));
	k_mutex_unlock(&m_lock_metrics);

	return 0;
}

int hio_cloud_transfer_uplink(struct hio_buf *buf, bool *has_downlink, k_timeout_t timeout)
{
	int ret = 0;
	int res = 0;
	uint8_t *p = NULL;
	size_t len = 0;
	int part = 0;
	int fragments = 0;

	if (has_downlink) {
		*has_downlink = false;
	}

	size_t max_data_size = transfer_mode_max_data_size();
	if (!max_data_size) {
		LOG_ERR("Invalid max data size");
		return -EIO;
	}

restart:
	part = 0;

	if (buf) {
		p = hio_buf_get_mem(buf);
		len = hio_buf_get_used(buf);

		/* calculate number of fragments */
		fragments = len / max_data_size;
		if (len % max_data_size) {
			fragments++;
		}
	}

	do {
		LOG_INF("Processing part: %d (%d left)", part, fragments - part - 1);

		m_pck_send.data = p;
		m_pck_send.data_len = MIN(len, max_data_size);
		m_pck_send.sequence = m_sequence;
		m_sequence = hio_cloud_packet_sequence_inc(m_sequence);

		m_pck_send.flags = 0;
		if (part == 0) {
			m_pck_send.flags |= HIO_CLOUD_PACKET_FLAG_FIRST;
		}

		if (len == m_pck_send.data_len) {
			m_pck_send.flags |= HIO_CLOUD_PACKET_FLAG_LAST;
		}

		bool rai = m_pck_send.flags & HIO_CLOUD_PACKET_FLAG_LAST;

		int repeats = 0;
	resend_uplink:
		ret = transfer(&m_pck_send, &m_pck_recv, rai, timeout);
		if (ret == TRANSFER_ADDR_SWITCHED) {
			LOG_WRN("Address switched, restarting uplink from first part");
			m_sequence = 0;
			m_last_recv_sequence = 0;
			goto restart;
		}
		if (ret) {
			LOG_ERR("Call `transfer` failed: %d", ret);
			res = ret;
			goto exit;
		}

		if (has_downlink) {
			*has_downlink = m_pck_recv.flags & HIO_CLOUD_PACKET_FLAG_POLL;
		}

		if (m_pck_recv.flags & (HIO_CLOUD_PACKET_FLAG_FIRST | HIO_CLOUD_PACKET_FLAG_LAST)) {
			LOG_ERR("Received unexpected flags");
			res = -EIO;
			goto exit;
		}

		if (m_pck_recv.sequence == 0) {
			LOG_WRN("Received sequence reset request");
			m_sequence = 0;
			goto restart;
		}

		if (m_pck_recv.sequence != m_sequence) {
			if (m_pck_recv.sequence == m_last_recv_sequence) {
				/* Stale ACK from a prior exchange (the server's reply
				 * lags by one window under RAI). Re-send the SAME
				 * fragment with the SAME sequence and wait for the
				 * matching ACK — do not advance the sequence, which
				 * would desync the server. */
				if (++repeats <= TRANSFER_REPEAT_LIMIT) {
					LOG_WRN("Received repeat response, resending part %d (%d/%d)",
						part, repeats, TRANSFER_REPEAT_LIMIT);
					goto resend_uplink;
				}
				LOG_WRN("Repeat response limit reached, restarting transfer");
				m_sequence = 0;
				goto restart;
			} else {
				LOG_WRN("Received unexpected sequence expect: %u", m_sequence);
				m_sequence = 0;
				goto restart;
			}
		}

		if (m_pck_recv.data_len) {
			LOG_ERR("Received unexpected data length");
			m_sequence = 0;
			goto restart;
		}

		m_last_recv_sequence = m_pck_recv.sequence;
		m_sequence = hio_cloud_packet_sequence_inc(m_sequence);

		p += m_pck_send.data_len;
		len -= m_pck_send.data_len;
		part++;

	} while (len);

exit:
	if (res) {
		LOG_ERR("Transfer uplink failed: %d reset sequence", res);
		m_sequence = 0;
		m_last_recv_sequence = 0;
	}

	k_mutex_lock(&m_lock_metrics, K_FOREVER);
	if (res) {
		m_metrics.uplink_errors++;
	} else {
		m_metrics.uplink_count++;
		m_metrics.uplink_bytes += hio_buf_get_used(buf);
		m_metrics.uplink_fragments += fragments;
		m_metrics.uplink_last_ts = time(NULL);
	}
	k_mutex_unlock(&m_lock_metrics);

	return res;
}

int hio_cloud_transfer_downlink(struct hio_buf *buf, bool *has_downlink, k_timeout_t timeout)
{
	int ret;
	int res = 0;
	int part = 0;
	bool quit;

	if (has_downlink) {
		*has_downlink = false;
	}

	size_t buf_used = hio_buf_get_used(buf);

restart:

	part = 0;
	quit = false;

	do {
		LOG_INF("Processing part: %d", part);

		m_pck_send.flags =
			part == 0 ? HIO_CLOUD_PACKET_FLAG_POLL : HIO_CLOUD_PACKET_FLAG_ACK;
		m_pck_send.data = NULL;
		m_pck_send.data_len = 0;
		m_pck_send.sequence = m_sequence;
		m_sequence = hio_cloud_packet_sequence_inc(m_sequence);

		int repeats = 0;
	again:
		if (quit) {
			bool rai = has_downlink ? !*has_downlink
						: true; /* use RAI if no downlink is expected */
			ret = transfer(&m_pck_send, NULL, rai, timeout);
			if (ret == TRANSFER_ADDR_SWITCHED) {
				LOG_WRN("Address switched, restarting downlink");
				m_sequence = 0;
				m_last_recv_sequence = 0;
				goto restart;
			}
			if (ret) {
				LOG_ERR("Call `transfer` failed: %d", ret);
				res = ret;
				goto exit;
			}
			break;
		}

		bool rai = part == 0;

		LOG_INF("Downlink: Starting send_recv");

		ret = transfer(&m_pck_send, &m_pck_recv, rai, timeout);
		if (ret == TRANSFER_ADDR_SWITCHED) {
			LOG_WRN("Address switched, restarting downlink");
			m_sequence = 0;
			m_last_recv_sequence = 0;
			goto restart;
		}
		if (ret) {
			LOG_ERR("Call `transfer` failed: %d", ret);
			res = ret;
			goto exit;
		}

		if (m_pck_recv.sequence == 0) {
			LOG_WRN("Received sequence reset request");
			m_sequence = 0;
			goto restart;
		}

		if (m_pck_recv.sequence != m_sequence) {
			if (m_pck_recv.sequence == m_last_recv_sequence) {
				/* Stale ACK lagging by one window: re-send the same
				 * poll/ack with the same sequence (goto again keeps
				 * m_pck_send untouched) and wait for the matching
				 * reply, bounded so a persistently stale peer cannot
				 * spin forever. */
				if (++repeats <= TRANSFER_REPEAT_LIMIT) {
					LOG_WRN("Received repeat response, resending (%d/%d)",
						repeats, TRANSFER_REPEAT_LIMIT);
					goto again;
				}
				LOG_WRN("Repeat response limit reached, restarting transfer");
				m_sequence = 0;
				goto restart;
			} else {
				LOG_WRN("Received unexpected sequence expect: %u", m_sequence);
				m_sequence = 0;
				goto restart;
			}
		}

		m_last_recv_sequence = m_pck_recv.sequence;
		m_sequence = hio_cloud_packet_sequence_inc(m_sequence);

		if (m_pck_recv.flags & HIO_CLOUD_PACKET_FLAG_ACK) {
			LOG_ERR("Received unexpected flags");
			res = -EIO;
			goto exit;
		}

		if (m_pck_recv.flags & HIO_CLOUD_PACKET_FLAG_FIRST) {
			hio_buf_reset(buf);
		}

		ret = hio_buf_append_mem(buf, m_pck_recv.data, m_pck_recv.data_len);
		if (ret) {
			LOG_ERR("Call `hio_buf_append_mem` failed: %d", ret);
			res = ret;
			goto exit;
		}

		quit = m_pck_recv.flags & HIO_CLOUD_PACKET_FLAG_LAST;

		if (has_downlink) {
			*has_downlink = m_pck_recv.flags & HIO_CLOUD_PACKET_FLAG_POLL;
		}

		if (quit && m_pck_recv.data_len == 0 && part == 0) {
			LOG_INF("Skip ack response");
			break;
		}

		part++;

	} while (true);

exit:

	if (res) {
		LOG_ERR("Transfer downlink failed: %d reset sequence", res);
		m_sequence = 0;
		m_last_recv_sequence = 0;
	}

	k_mutex_lock(&m_lock_metrics, K_FOREVER);
	if (res) {
		m_metrics.downlink_errors++;
	} else {
		if (part) {
			m_metrics.downlink_count++;
			m_metrics.downlink_fragments += part;
			m_metrics.downlink_bytes += hio_buf_get_used(buf) - buf_used;
			m_metrics.downlink_last_ts = time(NULL);
		} else {
			m_metrics.poll_count++;
			m_metrics.poll_last_ts = time(NULL);
		}
	}
	k_mutex_unlock(&m_lock_metrics);

	return res;
}

int hio_cloud_transfer_set_psk(const char *psk_hex)
{
	const char *serial_number_str;
	int ret = hio_info_get_serial_number(&serial_number_str);
	if (ret) {
		LOG_ERR("Read serial number failed: %d", ret);
		return ret;
	}

	char identity[4 + strlen(serial_number_str) + 1];

	snprintf(identity, sizeof(identity), "hsn:%s", serial_number_str);

	return hio_lte_set_psk(identity, psk_hex);
}

static int hio_cloud_transfer_get_failover_state(struct hio_cloud_backend_failover_state *state)
{
	if (!state) {
		return -EINVAL;
	}

	const char *addrs[3];
	int count = build_addr_list(addrs);

	int idx = m_active_idx;

	state->active_idx = idx;
	state->active_addr = (count > 0 && idx < count) ? addrs[idx] : "";
	state->consecutive_failures = m_consecutive_failures;
	state->failover_count = m_failover_count;

	return 0;
}

const struct hio_cloud_backend hio_cloud_backend_udp_lte = {
	.init = hio_cloud_transfer_init,
	.wait_ready = hio_cloud_transfer_wait_for_ready,
	.uplink = hio_cloud_transfer_uplink,
	.downlink = hio_cloud_transfer_downlink,
	.set_psk = hio_cloud_transfer_set_psk,
	.get_metrics = hio_cloud_transfer_get_metrics,
	.reset_metrics = hio_cloud_transfer_reset_metrics,
	.get_failover_state = hio_cloud_transfer_get_failover_state,
};
