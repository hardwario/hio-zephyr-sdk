/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/*
 * Retries of hio_cloud_transfer_uplink() and hio_cloud_transfer_downlink() when an exchange fails
 * or gets a stale response, over a scripted hio_lte_send_recv()
 */

#include "hio_cloud_backend.h"
#include "hio_cloud_config.h"
#include "hio_cloud_packet.h"
#include "hio_cloud_transfer.h"
#include "mock.h"

/* HIO includes */
#include <hio/hio_buf.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define SERIAL_NUMBER 2159018247

#define UPLINK (HIO_CLOUD_PACKET_FLAG_FIRST | HIO_CLOUD_PACKET_FLAG_LAST)
#define POLL   HIO_CLOUD_PACKET_FLAG_POLL
#define ACK    HIO_CLOUD_PACKET_FLAG_ACK
#define FIRST  HIO_CLOUD_PACKET_FLAG_FIRST
#define LAST   HIO_CLOUD_PACKET_FLAG_LAST
#define DATA   (HIO_CLOUD_PACKET_FLAG_FIRST | HIO_CLOUD_PACKET_FLAG_LAST)

#define ADDR  "192.0.2.1"
#define ADDR2 "192.0.2.2"

/* clang-format off */
/* The exchange of the packet fails after ms */
#define FAIL(seq, fl, err, ms) \
	{.sequence = (seq), .flags = (fl), .ret = (err), .duration_ms = (ms)}
/* The exchange of the packet fails ms after its timeout */
#define FAIL_LATE(seq, fl, err, ms) \
	{.sequence = (seq), .flags = (fl), .ret = (err), .duration_ms = MOCK_LTE_UNTIL_TIMEOUT, \
	 .late_ms = (ms)}
/* The packet gets the response at once */
#define REPLY(seq, fl, rseq, rfl, data) \
	{.sequence = (seq), .flags = (fl), .resp_sequence = (rseq), .resp_flags = (rfl), \
	 .resp_data = (data)}
/* The packet gets the response after ms */
#define REPLY_AFTER(seq, fl, rseq, rfl, data, ms) \
	{.sequence = (seq), .flags = (fl), .resp_sequence = (rseq), .resp_flags = (rfl), \
	 .resp_data = (data), .duration_ms = (ms)}
/* The final ACK of a downlink, which expects no response, fails after ms */
#define ACK_FAIL(seq, err, ms) \
	{.sequence = (seq), .flags = ACK, .no_recv = true, .ret = (err), .duration_ms = (ms)}
#define ACK_OK(seq) \
	{.sequence = (seq), .flags = ACK, .no_recv = true}
/* clang-format on */

/* hio_cloud_config.c is not compiled */
struct hio_cloud_config g_hio_cloud_config;

static const uint8_t m_token[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
				    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};

HIO_BUF_DEFINE_STATIC(m_buf, 64);
/* Two fragments: 492 and 108 bytes */
HIO_BUF_DEFINE_STATIC(m_big_buf, 600);

static int uplink(k_timeout_t timeout, int64_t *elapsed)
{
	hio_buf_reset(&m_buf);
	zassert_ok(hio_buf_append_mem(&m_buf, "Hello", 5));

	int64_t start = k_uptime_get();
	int ret = hio_cloud_transfer_uplink(&m_buf, NULL, timeout);
	*elapsed = k_uptime_get() - start;

	return ret;
}

static int downlink(k_timeout_t timeout, int64_t *elapsed)
{
	hio_buf_reset(&m_buf);

	int64_t start = k_uptime_get();
	int ret = hio_cloud_transfer_downlink(&m_buf, NULL, timeout);
	*elapsed = k_uptime_get() - start;

	return ret;
}

static void assert_downlink_data(const char *data)
{
	size_t len = strlen(data);

	zassert_equal(hio_buf_get_used(&m_buf), len, "downlink of %zu bytes",
		      hio_buf_get_used(&m_buf));
	zassert_mem_equal(hio_buf_get_mem(&m_buf), data, len);
}

/* Assert that the call started at_ms after the first one, with the timeout; each sleep before it
 * may end a tick or so late */
static void assert_call(size_t index, int64_t at_ms, int64_t timeout_ms)
{
	const struct mock_lte_call *call = mock_lte_call(index);
	int64_t at = call->start - mock_lte_call(0)->start;
	int64_t delta = 20 * (index + 1);

	zassert_within(at, at_ms, delta, "call %zu at %lld ms, expected %lld ms", index, at, at_ms);

	if (timeout_ms == MOCK_LTE_FOREVER) {
		zassert_equal(call->timeout_ms, MOCK_LTE_FOREVER,
			      "call %zu with timeout %lld ms, expected K_FOREVER", index,
			      call->timeout_ms);
	} else {
		zassert_within(call->timeout_ms, timeout_ms, delta,
			       "call %zu with timeout %lld ms, expected %lld ms", index,
			       call->timeout_ms, timeout_ms);
	}
}

/* Assert the consecutive failures the address failover had counted before the call */
static void assert_failures(size_t index, int failures)
{
	zassert_equal(mock_lte_call(index)->failures, failures,
		      "call %zu after %d failures, expected %d", index,
		      mock_lte_call(index)->failures, failures);
}

static void get_failover_state(struct hio_cloud_backend_failover_state *state)
{
	zassert_ok(hio_cloud_backend_udp_lte.get_failover_state(state));
}

/* A lost response: the same packet again 1 s later, with the time left */
ZTEST(transfer, test_lost_reply)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ETIMEDOUT, 5000),
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	assert_call(0, 0, 60000);
	assert_call(1, 6000, 54000);
	mock_lte_assert_done();
}

/* No connection granted: the same packet again 30 s later */
ZTEST(transfer, test_no_conn)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ENOTCONN, 3000),
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(120), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	assert_call(1, 33000, 87000);
	mock_lte_assert_done();
}

/* Each attempt gets the time left, and the backoff ends at the deadline */
ZTEST(transfer, test_deadline)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ETIMEDOUT, 5000),
		/* The backoff of 30 s is cut to the 29 s left */
		FAIL(0, UPLINK, -ENOTCONN, 10000),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(45), &elapsed);
	zassert_equal(ret, -ENOTCONN, "expected -ENOTCONN but got: %d", ret);
	zassert_within(elapsed, 45000, 20, "returned %lld ms after the call", elapsed);

	assert_call(0, 0, 45000);
	assert_call(1, 6000, 39000);
	mock_lte_assert_done();
}

/* Attempts that last until their timeout: the last one ends at the deadline */
ZTEST(transfer, test_deadline_lost_replies)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ENOTCONN, 4000),
		/* After the backoff of 30 s, 6 s are left */
		FAIL(0, UPLINK, -ETIMEDOUT, MOCK_LTE_UNTIL_TIMEOUT),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(40), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 40000, 20, "returned %lld ms after the call", elapsed);

	assert_call(0, 0, 40000);
	assert_call(1, 34000, 6000);
	mock_lte_assert_done();
}

/* The backoff after no connection ends at the deadline, without another attempt */
ZTEST(transfer, test_no_conn_backoff_cut)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ENOTCONN, 1000),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(10), &elapsed);
	zassert_equal(ret, -ENOTCONN, "expected -ENOTCONN but got: %d", ret);
	zassert_within(elapsed, 10000, 20, "returned %lld ms after the call", elapsed);

	mock_lte_assert_done();
}

/* The final ACK of a downlink, which gets no response, is sent again after no connection, and
 * the failover counts the failed attempt */
ZTEST(transfer, test_ack_no_conn)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_FAIL(2, -ENOTCONN, 2000),
		ACK_OK(2),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	assert_downlink_data("abc");

	assert_call(2, 32000, 28000);
	assert_failures(1, 0);
	assert_failures(2, 1);
	mock_lte_assert_done();

	struct hio_cloud_backend_failover_state state;
	get_failover_state(&state);
	zassert_equal(state.consecutive_failures, 0, "%d failures after the ACK",
		      state.consecutive_failures);
}

/* ... but not after a lost response, as it expects none; the LTE layer reports one at the
 * deadline. All the data is in, so the downlink keeps it, and the next packet starts again at
 * sequence 0, which the server takes as the missed ACK */
ZTEST(transfer, test_ack_lost_reply)
{
	static const struct mock_lte_exchange script[] = {
		REPLY_AFTER(0, POLL, 1, DATA, "abc", 5000),
		ACK_FAIL(2, -ETIMEDOUT, MOCK_LTE_UNTIL_TIMEOUT),
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	zassert_within(elapsed, 60000, 20, "returned %lld ms after the call", elapsed);
	assert_downlink_data("abc");

	assert_call(1, 5000, 55000);

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);
	mock_lte_assert_done();
}

/* A lost response to the final ACK before the deadline, which the LTE layer does not report
 * today, is not asked for again either */
ZTEST(transfer, test_ack_lost_reply_early)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_FAIL(2, -ETIMEDOUT, 2000),
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	zassert_within(elapsed, 2000, 20, "returned %lld ms after the call", elapsed);
	assert_downlink_data("abc");

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);
	mock_lte_assert_done();
}

/* No connection for the final ACK until the deadline: the downlink keeps the data too */
ZTEST(transfer, test_ack_no_conn_deadline)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_FAIL(2, -ENOTCONN, 2000),
		ACK_FAIL(2, -ENOTCONN, MOCK_LTE_UNTIL_TIMEOUT),
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_SECONDS(40), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	zassert_within(elapsed, 40000, 20, "returned %lld ms after the call", elapsed);
	assert_downlink_data("abc");

	assert_call(2, 32000, 8000);

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);
	mock_lte_assert_done();
}

/* Less than the floor left for the final ACK: it is not sent, and the downlink keeps the data */
ZTEST(transfer, test_ack_past_deadline)
{
	static const struct mock_lte_exchange script[] = {
		REPLY_AFTER(0, POLL, 1, DATA, "abc", 3000),
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_MSEC(7000), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	zassert_within(elapsed, 3000, 20, "returned %lld ms after the call", elapsed);
	assert_downlink_data("abc");

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);
	mock_lte_assert_done();
}

/* An attempt with less than the floor left is not made, nor counted by the failover */
ZTEST(transfer, test_attempt_under_floor)
{
	static const struct mock_lte_exchange script[] = {
		/* After the backoff of 1 s, 4 s are left */
		FAIL(0, UPLINK, -ETIMEDOUT, 5000),
	};

	strcpy(g_hio_cloud_config.addr2, ADDR2);
	g_hio_cloud_config.failover = 2;

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(10), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 6000, 20, "returned %lld ms after the call", elapsed);

	mock_lte_assert_done();

	const char *addr;
	zassert_equal(mock_lte_switches(&addr), 0, "%d switches", mock_lte_switches(&addr));

	struct hio_cloud_backend_failover_state state;
	get_failover_state(&state);
	zassert_equal(state.active_idx, 0, "active address %d", state.active_idx);
	zassert_equal(state.consecutive_failures, 1, "%d failures", state.consecutive_failures);
}

/* With no time, nothing is sent */
ZTEST(transfer, test_no_wait)
{
	mock_lte_start(SERIAL_NUMBER, m_token, NULL, 0);

	int64_t elapsed;
	int ret = uplink(K_NO_WAIT, &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_equal(elapsed, 0, "returned %lld ms after the call", elapsed);

	ret = downlink(K_NO_WAIT, &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_equal(elapsed, 0, "returned %lld ms after the call", elapsed);

	mock_lte_assert_done();
}

/* A call that returns past its timeout, as the real one does after its IO section, is the last
 * one: neither a backoff nor another attempt follows */
ZTEST(transfer, test_overrun)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ETIMEDOUT, 4000),
		/* Started with 6 s left, it returns 3 s past the deadline */
		FAIL_LATE(0, UPLINK, -ETIMEDOUT, 3000),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(11), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 14000, 20, "returned %lld ms after the call", elapsed);

	assert_call(1, 5000, 6000);
	mock_lte_assert_done();
}

/* -EBUSY and -ENOTSUP are local refusals: neither retried nor counted by the failover. On the
 * final ACK of a downlink, the data is kept */
ZTEST(transfer, test_local_refusals)
{
	/* clang-format off */
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -EBUSY, 0),
		FAIL(0, UPLINK, -ENOTSUP, 0),
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_FAIL(2, -EBUSY, 0),
		REPLY(0, UPLINK, 1, 0, NULL),
	};
	/* clang-format on */

	strcpy(g_hio_cloud_config.addr2, ADDR2);
	g_hio_cloud_config.failover = 1;

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(60), &elapsed);
	zassert_equal(ret, -EBUSY, "expected -EBUSY but got: %d", ret);

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_equal(ret, -ENOTSUP, "expected -ENOTSUP but got: %d", ret);

	ret = downlink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	assert_downlink_data("abc");

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	assert_failures(1, 0);
	assert_failures(2, 0);
	assert_failures(3, 0);
	assert_failures(4, 0);
	mock_lte_assert_done();

	const char *addr;
	zassert_equal(mock_lte_switches(&addr), 0, "%d switches", mock_lte_switches(&addr));
}

/* The fragments of an uplink share its deadline: the second one gets the time left, or with less
 * than the floor left is not sent at all, nor counted by the failover */
ZTEST(transfer, test_fragment_past_deadline)
{
	static const struct mock_lte_exchange script[] = {
		REPLY_AFTER(0, FIRST, 1, 0, NULL, 3000),
		FAIL(2, LAST, -ETIMEDOUT, MOCK_LTE_UNTIL_TIMEOUT),
		/* From sequence 0 again; 4 s left after the response */
		REPLY_AFTER(0, FIRST, 1, 0, NULL, 5000),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	hio_buf_reset(&m_big_buf);
	memset(hio_buf_get_mem(&m_big_buf), 0x55, hio_buf_get_free(&m_big_buf));
	zassert_ok(hio_buf_seek(&m_big_buf, 600));

	int64_t start = k_uptime_get();
	int ret = hio_cloud_transfer_uplink(&m_big_buf, NULL, K_MSEC(9000));
	int64_t elapsed = k_uptime_get() - start;

	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 9000, 20, "returned %lld ms after the call", elapsed);
	assert_call(1, 3000, 6000);

	start = k_uptime_get();
	ret = hio_cloud_transfer_uplink(&m_big_buf, NULL, K_MSEC(9000));
	elapsed = k_uptime_get() - start;

	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 5000, 20, "returned %lld ms after the call", elapsed);

	mock_lte_assert_done();

	struct hio_cloud_backend_failover_state state;
	get_failover_state(&state);
	zassert_equal(state.consecutive_failures, 0, "%d failures", state.consecutive_failures);
}

/* A stale response to an uplink: the same packet with the same sequence, at most 3 times, then a
 * restart 1 s later */
ZTEST(transfer, test_stale_uplink)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, UPLINK, 1, 0, NULL),
		/* The response to the previous packet, 4 times */
		REPLY(2, UPLINK, 1, 0, NULL),
		REPLY(2, UPLINK, 1, 0, NULL),
		REPLY(2, UPLINK, 1, 0, NULL),
		REPLY(2, UPLINK, 1, 0, NULL),
		/* Restarted */
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	assert_call(4, 0, 60000);
	assert_call(5, 1000, 59000);
	mock_lte_assert_done();
}

/* The same for a downlink */
ZTEST(transfer, test_stale_downlink)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, POLL, 1, LAST, NULL),
		/* The response to the previous packet, 4 times */
		REPLY(2, POLL, 1, LAST, NULL),
		REPLY(2, POLL, 1, LAST, NULL),
		REPLY(2, POLL, 1, LAST, NULL),
		REPLY(2, POLL, 1, LAST, NULL),
		/* Restarted */
		REPLY(0, POLL, 1, LAST, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);

	ret = downlink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);

	assert_call(4, 0, 60000);
	assert_call(5, 1000, 59000);
	mock_lte_assert_done();
}

/* A restart waits 1 s, and is not made with less than the floor left */
ZTEST(transfer, test_restart_deadline)
{
	static const struct mock_lte_exchange script[] = {
		/* An unexpected sequence each time */
		REPLY(0, UPLINK, 77, 0, NULL),
		REPLY(0, UPLINK, 77, 0, NULL),
		REPLY(0, UPLINK, 77, 0, NULL),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_MSEC(7500), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 3000, 50, "returned %lld ms after the call", elapsed);

	assert_call(1, 1000, 6500);
	assert_call(2, 2000, 5500);
	mock_lte_assert_done();
}

/* The same for a downlink; and the wait of a restart ends at the deadline */
ZTEST(transfer, test_restart_deadline_downlink)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, POLL, 77, LAST, NULL),
		REPLY(0, POLL, 77, LAST, NULL),
		REPLY(0, POLL, 77, LAST, NULL),
		/* 500 ms left after the response */
		REPLY_AFTER(0, POLL, 77, LAST, NULL, 7000),
	};

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_MSEC(7500), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 3000, 50, "returned %lld ms after the call", elapsed);

	assert_call(1, 1000, 6500);
	assert_call(2, 2000, 5500);

	ret = downlink(K_MSEC(7500), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 7500, 20, "returned %lld ms after the call", elapsed);
	mock_lte_assert_done();
}

/* K_FOREVER retries on, the final ACK of a downlink too */
ZTEST(transfer, test_forever)
{
	/* clang-format off */
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ENOTCONN, 1000),
		FAIL(0, UPLINK, -ENOTCONN, 1000),
		FAIL(0, UPLINK, -ENOTCONN, 1000),
		FAIL(0, UPLINK, -ETIMEDOUT, 5000),
		FAIL(0, UPLINK, -ETIMEDOUT, 5000),
		REPLY(0, UPLINK, 1, 0, NULL),
		REPLY(2, POLL, 3, DATA, "abc"),
		ACK_FAIL(4, -ENOTCONN, 1000),
		ACK_FAIL(4, -ENOTCONN, 1000),
		ACK_OK(4),
	};
	/* clang-format on */

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_FOREVER, &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	ret = downlink(K_FOREVER, &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	assert_downlink_data("abc");

	assert_call(0, 0, MOCK_LTE_FOREVER);
	assert_call(1, 31000, MOCK_LTE_FOREVER);
	assert_call(2, 62000, MOCK_LTE_FOREVER);
	assert_call(3, 93000, MOCK_LTE_FOREVER);
	assert_call(4, 99000, MOCK_LTE_FOREVER);
	assert_call(5, 105000, MOCK_LTE_FOREVER);
	assert_call(6, 105000, MOCK_LTE_FOREVER);
	assert_call(7, 105000, MOCK_LTE_FOREVER);
	assert_call(8, 136000, MOCK_LTE_FOREVER);
	assert_call(9, 167000, MOCK_LTE_FOREVER);
	mock_lte_assert_done();
}

/* The failover counts every attempt and switches the address after 3 failures in a row; the
 * uplink restarts against the new address 1 s later, with the time left */
ZTEST(transfer, test_failover_counts_attempts)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ETIMEDOUT, 2000),
		FAIL(0, UPLINK, -ETIMEDOUT, 2000),
		FAIL(0, UPLINK, -ETIMEDOUT, 2000),
		/* Restarted against ADDR2 */
		REPLY(0, UPLINK, 1, 0, NULL),
	};

	strcpy(g_hio_cloud_config.addr2, ADDR2);
	g_hio_cloud_config.failover = 3;

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(60), &elapsed);
	zassert_ok(ret, "uplink failed: %d", ret);

	assert_call(0, 0, 60000);
	assert_call(1, 3000, 57000);
	assert_call(2, 6000, 54000);
	assert_call(3, 9000, 51000);
	assert_failures(0, 0);
	assert_failures(1, 1);
	assert_failures(2, 2);
	assert_failures(3, 0);
	mock_lte_assert_done();

	const char *addr;
	zassert_equal(mock_lte_switches(&addr), 1, "%d switches", mock_lte_switches(&addr));
	zassert_str_equal(addr, ADDR2);

	struct hio_cloud_backend_failover_state state;
	get_failover_state(&state);
	zassert_equal(state.active_idx, 1, "active address %d", state.active_idx);
	zassert_equal(state.failover_count, 1, "%u switches", state.failover_count);
	zassert_equal(state.consecutive_failures, 0, "%d failures", state.consecutive_failures);
}

/* The retries of the final ACK count too. The server at the new address knows nothing of the
 * downlink and delivers it again at the next poll, so a switch during them drops the data with
 * an error, without a restart */
ZTEST(transfer, test_failover_ack)
{
	static const struct mock_lte_exchange script[] = {
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_FAIL(2, -ENOTCONN, 1000),
		ACK_FAIL(2, -ENOTCONN, 1000),
		/* The next poll, against ADDR2 */
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_OK(2),
	};

	strcpy(g_hio_cloud_config.addr2, ADDR2);
	g_hio_cloud_config.failover = 2;

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = downlink(K_SECONDS(120), &elapsed);
	zassert_equal(ret, -ECONNRESET, "expected -ECONNRESET but got: %d", ret);
	zassert_within(elapsed, 32000, 50, "returned %lld ms after the call", elapsed);
	zassert_equal(hio_buf_get_used(&m_buf), 0, "downlink of %zu bytes kept",
		      hio_buf_get_used(&m_buf));

	assert_call(2, 31000, 89000);
	assert_failures(1, 0);
	assert_failures(2, 1);

	const char *addr;
	zassert_equal(mock_lte_switches(&addr), 1, "%d switches", mock_lte_switches(&addr));
	zassert_str_equal(addr, ADDR2);

	ret = downlink(K_SECONDS(120), &elapsed);
	zassert_ok(ret, "downlink failed: %d", ret);
	assert_downlink_data("abc");
	assert_failures(3, 0);
	mock_lte_assert_done();
}

/* The address switches on an attempt that ends at the deadline: the uplink fails without another
 * attempt, and a downlink whose final ACK switches it drops the data */
ZTEST(transfer, test_switch_at_deadline)
{
	static const struct mock_lte_exchange script[] = {
		FAIL(0, UPLINK, -ETIMEDOUT, 4000),
		FAIL(0, UPLINK, -ETIMEDOUT, MOCK_LTE_UNTIL_TIMEOUT),
		REPLY(0, POLL, 1, DATA, "abc"),
		ACK_FAIL(2, -ENOTCONN, MOCK_LTE_UNTIL_TIMEOUT),
	};

	strcpy(g_hio_cloud_config.addr2, ADDR2);
	g_hio_cloud_config.failover = 2;

	mock_lte_start(SERIAL_NUMBER, m_token, script, ARRAY_SIZE(script));

	int64_t elapsed;
	int ret = uplink(K_SECONDS(11), &elapsed);
	zassert_equal(ret, -ETIMEDOUT, "expected -ETIMEDOUT but got: %d", ret);
	zassert_within(elapsed, 11000, 20, "returned %lld ms after the call", elapsed);

	const char *addr;
	zassert_equal(mock_lte_switches(&addr), 1, "%d switches", mock_lte_switches(&addr));
	zassert_str_equal(addr, ADDR2);

	g_hio_cloud_config.failover = 1;

	ret = downlink(K_SECONDS(20), &elapsed);
	zassert_equal(ret, -ECONNRESET, "expected -ECONNRESET but got: %d", ret);
	zassert_within(elapsed, 20000, 20, "returned %lld ms after the call", elapsed);
	zassert_equal(hio_buf_get_used(&m_buf), 0, "downlink of %zu bytes kept",
		      hio_buf_get_used(&m_buf));
	mock_lte_assert_done();

	zassert_equal(mock_lte_switches(&addr), 2, "%d switches", mock_lte_switches(&addr));
	zassert_str_equal(addr, ADDR);
}

static void before(void *fixture)
{
	memset(&g_hio_cloud_config, 0, sizeof(g_hio_cloud_config));
	g_hio_cloud_config.protocol = HIO_CLOUD_PROTOCOL_FLAP_HASH;
	strcpy(g_hio_cloud_config.addr, ADDR);
	g_hio_cloud_config.port_signed = 5002;
	g_hio_cloud_config.port_dtls = 5003;

	/* From sequence 0, no failures counted */
	zassert_ok(hio_cloud_transfer_init(SERIAL_NUMBER, m_token));
}

ZTEST_SUITE(transfer, NULL, NULL, before, NULL, NULL);
