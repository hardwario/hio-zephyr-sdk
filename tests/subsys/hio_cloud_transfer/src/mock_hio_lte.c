/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "mock.h"

#include "hio_cloud_backend.h"
#include "hio_cloud_packet.h"

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_info.h>
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CALLS_MAX 16

HIO_BUF_DEFINE_STATIC(m_pck_buf, HIO_LTE_UDP_MAX_MTU);

static uint32_t m_serial_number;
static uint8_t m_token[16];
static const struct mock_lte_exchange *m_exchanges;
static size_t m_count;
static size_t m_index;
static struct mock_lte_call m_calls[CALLS_MAX];
static int m_switches;
static char m_switch_addr[40];

void mock_lte_start(uint32_t serial_number, const uint8_t token[16],
		    const struct mock_lte_exchange *exchanges, size_t count)
{
	zassert_true(count <= CALLS_MAX, "script of %zu exchanges, at most %d", count, CALLS_MAX);

	m_serial_number = serial_number;
	memcpy(m_token, token, sizeof(m_token));
	m_exchanges = exchanges;
	m_count = count;
	m_index = 0;
	memset(m_calls, 0, sizeof(m_calls));
	m_switches = 0;
	m_switch_addr[0] = '\0';
}

const struct mock_lte_call *mock_lte_call(size_t index)
{
	zassert_true(index < m_index, "call %zu not made, %zu calls", index, m_index);

	return &m_calls[index];
}

void mock_lte_assert_done(void)
{
	zassert_equal(m_index, m_count, "script stopped at exchange %zu of %zu", m_index, m_count);
}

int mock_lte_switches(const char **addr)
{
	*addr = m_switch_addr;

	return m_switches;
}

/* hio_cloud_util.c needs the settings and the shell, so the packet hash is a stand-in (FNV-1a),
 * the same for the packets sent and the responses */
int hio_cloud_calculate_hash(uint8_t hash[8], const uint8_t *buf1, size_t len1, const uint8_t *buf2,
			     size_t len2)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	for (size_t i = 0; i < len1 + len2; i++) {
		h ^= i < len1 ? buf1[i] : buf2[i - len1];
		h *= 0x100000001b3ULL;
	}

	sys_put_be64(h, hash);

	return 0;
}

int hio_info_get_serial_number(const char **serial_number)
{
	*serial_number = "2159018247";

	return 0;
}

int hio_lte_enable(const struct hio_lte_socket_config *socket_config)
{
	return 0;
}

int hio_lte_disable(void)
{
	return 0;
}

int hio_lte_wait_for_disable(k_timeout_t timeout)
{
	return 0;
}

int hio_lte_wait_for_connected(k_timeout_t timeout)
{
	return 0;
}

int hio_lte_set_psk(const char *identity, const char *psk_hex)
{
	return 0;
}

int hio_lte_get_socket_mtu(size_t *mtu)
{
	*mtu = HIO_LTE_UDP_MAX_MTU;

	return 0;
}

int hio_lte_update_socket_config(const struct hio_lte_socket_config *socket_config)
{
	m_switches++;
	strncpy(m_switch_addr, socket_config->addr, sizeof(m_switch_addr) - 1);

	return 0;
}

static int64_t timeout_ms(k_timeout_t timeout)
{
	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		return MOCK_LTE_FOREVER;
	}

	return k_ticks_to_ms_floor64(timeout.ticks);
}

int hio_lte_send_recv(const struct hio_lte_send_recv_param *param)
{
	size_t i = m_index;

	zassert_true(i < m_count, "send_recv %zu past the end of the script", i);

	const struct mock_lte_exchange *ex = &m_exchanges[i];
	struct mock_lte_call *call = &m_calls[i];
	struct hio_cloud_backend_failover_state state;

	m_index++;

	call->start = k_uptime_get();
	call->timeout_ms = timeout_ms(param->timeout);
	zassert_true(call->timeout_ms == MOCK_LTE_FOREVER ||
			     call->timeout_ms >= MOCK_LTE_TIMEOUT_MIN_MS,
		     "exchange %zu: timeout %lld ms", i, call->timeout_ms);
	zassert_ok(hio_cloud_backend_udp_lte.get_failover_state(&state));
	call->failures = state.consecutive_failures;

	zassert_equal(param->retry_count, 0, "exchange %zu: retry_count %d", i, param->retry_count);

	/* The packet sent */
	struct hio_cloud_packet pck;
	uint32_t serial_number;

	hio_buf_reset(&m_pck_buf);
	zassert_ok(hio_buf_append_mem(&m_pck_buf, param->send_buf, param->send_len));
	zassert_ok(hio_cloud_packet_signed_unpack(&pck, &serial_number, m_token, &m_pck_buf));
	zassert_equal(serial_number, m_serial_number, "exchange %zu: serial number %u", i,
		      serial_number);
	zassert_equal(pck.sequence, ex->sequence, "exchange %zu: sequence %u, expected %u", i,
		      pck.sequence, ex->sequence);
	zassert_equal(pck.flags, ex->flags, "exchange %zu: flags 0x%x, expected 0x%x", i, pck.flags,
		      ex->flags);
	zassert_equal(param->recv_buf == NULL, ex->no_recv, "exchange %zu: recv_buf %p", i,
		      param->recv_buf);

	/* The call takes its time, but not more than its timeout */
	k_timepoint_t end = sys_timepoint_calc(param->timeout);
	k_timepoint_t done = end;

	if (ex->duration_ms != MOCK_LTE_UNTIL_TIMEOUT) {
		done = sys_timepoint_calc(K_MSEC(ex->duration_ms));
		if (sys_timepoint_cmp(done, end) > 0) {
			done = end;
		}
	} else {
		zassert_false(K_TIMEOUT_EQ(param->timeout, K_FOREVER),
			      "exchange %zu: until K_FOREVER", i);
	}

	k_sleep(sys_timepoint_timeout(done));

	if (ex->late_ms) {
		k_sleep(K_MSEC(ex->late_ms));
	}

	if (ex->ret) {
		return ex->ret;
	}

	*param->recv_len = 0;

	if (param->recv_buf) {
		struct hio_cloud_packet resp = {
			.sequence = ex->resp_sequence,
			.flags = ex->resp_flags,
			.data = (uint8_t *)ex->resp_data,
			.data_len = ex->resp_data ? strlen(ex->resp_data) : 0,
		};

		zassert_ok(
			hio_cloud_packet_signed_pack(&resp, m_serial_number, m_token, &m_pck_buf));
		zassert_true(hio_buf_get_used(&m_pck_buf) <= param->recv_size,
			     "exchange %zu: response of %zu bytes", i,
			     hio_buf_get_used(&m_pck_buf));
		memcpy(param->recv_buf, hio_buf_get_mem(&m_pck_buf), hio_buf_get_used(&m_pck_buf));
		*param->recv_len = hio_buf_get_used(&m_pck_buf);
	}

	return 0;
}
