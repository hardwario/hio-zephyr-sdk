/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

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
#include <time.h>

#define MODEM_SLEEP_DELAY K_SECONDS(10)

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
static int transfer(struct hio_cloud_packet *pck_send, struct hio_cloud_packet *pck_recv, bool rai)
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

	len = 0;

	LOG_HEXDUMP_INF(hio_buf_get_mem(send_buf), hio_buf_get_used(send_buf),
			rai ? "Sending packet RAI:" : "Sending packet:");

	struct hio_lte_send_recv_param param = {
		.rai = rai,
		.send_buf = hio_buf_get_mem(send_buf),
		.send_len = hio_buf_get_used(send_buf),
		.recv_buf = NULL,
		.recv_size = 0,
		.recv_len = &len,
		.timeout = K_FOREVER,
	};

	if (pck_recv) {
		hio_buf_reset(recv_buf);
		param.recv_buf = hio_buf_get_mem(recv_buf);
		param.recv_size = hio_buf_get_free(recv_buf);
	}

	ret = hio_lte_send_recv(&param);
	if (ret) {
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

int hio_cloud_transfer_init(uint32_t serial_number, uint8_t token[16])
{
	memset(&m_pck_send, 0, sizeof(m_pck_send));
	memset(&m_pck_recv, 0, sizeof(m_pck_recv));

	m_serial_number = serial_number;
	memcpy(m_token, token, sizeof(m_token));

	m_sequence = 0;
	m_last_recv_sequence = 0;

	hio_cloud_transfer_reset_metrics();

	struct hio_lte_socket_config socket_config = {
		.addr = g_hio_cloud_config.addr,
	};
	switch (g_hio_cloud_config.protocol) {
	case HIO_CLOUD_PROTOCOL_FLAP_HASH:
		socket_config.dtls_enabled = false;
		socket_config.port = g_hio_cloud_config.port_signed;
		break;
	case HIO_CLOUD_PROTOCOL_FLAP_DTLS:
		socket_config.dtls_enabled = true;
		socket_config.port = g_hio_cloud_config.port_dtls;
		break;
	default:
		LOG_ERR("Unsupported cloud protocol");
		return -EPROTONOSUPPORT;
	}

	hio_lte_enable(&socket_config);

	return 0;
}

int hio_cloud_transfer_wait_for_ready(k_timeout_t timeout)
{
	int ret;

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

int hio_cloud_transfer_uplink(struct hio_buf *buf, bool *has_downlink)
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
		ret = transfer(&m_pck_send, &m_pck_recv, rai);
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
				LOG_WRN("Received repeat response");
				continue;
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

int hio_cloud_transfer_downlink(struct hio_buf *buf, bool *has_downlink)
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

	again:
		if (quit) {
			bool rai = has_downlink ? !*has_downlink
						: true; /* use RAI if no downlink is expected */
			ret = transfer(&m_pck_send, NULL, rai);
			if (ret) {
				LOG_ERR("Call `transfer` failed: %d", ret);
				res = ret;
				goto exit;
			}
			break;
		}

		bool rai = part == 0;

		LOG_INF("Downlink: Starting send_recv");

		ret = transfer(&m_pck_send, &m_pck_recv, rai);
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
				LOG_WRN("Received repeat response");
				goto again;
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
