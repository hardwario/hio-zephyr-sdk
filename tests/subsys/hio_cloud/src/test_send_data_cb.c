/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* The callback API (hio_cloud_send_data_cb) and the reentrancy guard */

#include "fake_backend.h"

/* HIO includes */
#include <hio/hio_cloud.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

/* Third-party includes */
#include <zcbor_encode.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <string.h>

static K_SEM_DEFINE(m_entered, 0, 1);
static K_SEM_DEFINE(m_release, 0, 1);

static void *m_last_user_data;

/* Calls of the callback of the nested send_data_cb() */
static int m_inner_calls;

struct cb_ctx {
	int calls;
	int ret;           /* Returned by the callback */
	bool entered;      /* Give m_entered when called */
	bool release;      /* Then wait for m_release */
	k_timeout_t sleep; /* Sleep after encoding */
};

static int cb_map_1_42(zcbor_state_t *zs, void *user_data)
{
	struct cb_ctx *ctx = user_data;

	m_last_user_data = user_data;
	ctx->calls++;

	if (ctx->entered) {
		k_sem_give(&m_entered);
	}

	if (ctx->release) {
		/* Bounded, so that a broken test cannot hang the run */
		k_sem_take(&m_release, K_SECONDS(2));
	}

	zcbor_map_start_encode(zs, 1);
	zcbor_uint32_put(zs, 1);
	zcbor_uint32_put(zs, 42);
	zcbor_map_end_encode(zs, 1);

	if (!K_TIMEOUT_EQ(ctx->sleep, K_NO_WAIT)) {
		k_sleep(ctx->sleep);
	}

	return ctx->ret;
}

static void before(void *fixture)
{
	fake_before(fixture);
	k_sem_reset(&m_entered);
	k_sem_reset(&m_release);
	m_last_user_data = NULL;
	m_inner_calls = 0;
}

/* Nothing sent or fetched, also not by the poll worker */
static void assert_nothing_transferred(void)
{
	settle();
	zassert_equal(g_fake.uplink_count, 0);
	zassert_equal(g_fake.downlink_count, 0);
}

/* ---- Before hio_cloud_init() ---- */

ZTEST_SUITE(hio_cloud_send_data_cb_before_init, phase_before_init, NULL, before, NULL, NULL);

ZTEST(hio_cloud_send_data_cb_before_init, test_einval_before_init)
{
	/* Arguments are checked first */
	zassert_equal(hio_cloud_send_data_cb(NULL, NULL, K_FOREVER), -EINVAL);
}

ZTEST(hio_cloud_send_data_cb_before_init, test_eperm_before_init)
{
	struct cb_ctx ctx = {0};

	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER), -EPERM);
	zassert_equal(ctx.calls, 0);
	zassert_equal(g_fake.uplink_count, 0);
}

/* Runs after the *_before_init suites (alphabetical order) */
ZTEST_SUITE(hio_cloud_send_data_cb_failed_init, phase_before_init, NULL, before, NULL, NULL);

ZTEST(hio_cloud_send_data_cb_failed_init, test_eperm_after_failed_init)
{
	static struct hio_cloud_options options = {.decoder_hash = HASH};
	struct cb_ctx ctx = {0};

	g_fake.init_ret = -EIO;

	zassert_equal(hio_cloud_init(&options), -EIO);
	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER), -EPERM);
	zassert_equal(ctx.calls, 0);
	zassert_equal(g_fake.uplink_count, 0);
}

/* ---- After hio_cloud_init() ---- */

ZTEST_SUITE(hio_cloud_send_data_cb, phase_started, NULL, before, NULL, NULL);

ZTEST(hio_cloud_send_data_cb, test_cb_error_nothing_sent)
{
	struct cb_ctx ctx = {.ret = -EIO};

	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER), -EIO);
	zassert_equal(ctx.calls, 1);
	assert_nothing_transferred();
	assert_lock_free();
}

ZTEST(hio_cloud_send_data_cb, test_deadline_expired_after_encode)
{
	struct cb_ctx ctx = {.sleep = K_MSEC(100)};

	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_MSEC(50)), -ETIMEDOUT);
	zassert_equal(ctx.calls, 1);
	assert_nothing_transferred();
	assert_lock_free();
}

ZTEST(hio_cloud_send_data_cb, test_downlink_deferred)
{
	struct cb_ctx ctx = {0};

	g_fake.uplink_has_downlink = true;

	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER));
	zassert_equal(g_fake.uplink_count, 1);

	/* Fetched by the poll worker, not on the caller's stack */
	zassert_ok(fake_wait_downlink(K_SECONDS(1)));
	zassert_equal(g_fake.downlink_count, 1);
	zassert_not_equal(g_fake.downlink_thread, k_current_get());
}

static int send_cb_held(void *arg)
{
	return hio_cloud_send_data_cb(cb_map_1_42, arg, K_FOREVER);
}

ZTEST(hio_cloud_send_data_cb, test_eagain_lock_busy)
{
	struct cb_ctx held = {.entered = true, .release = true};
	struct cb_ctx ctx = {0};
	int ret = INT_MIN;

	/* Helper 0 holds the cloud lock in its callback */
	helper_start(0, send_cb_held, &held);
	zassert_ok(k_sem_take(&m_entered, K_SECONDS(1)));

	int64_t start = k_uptime_get();

	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_MSEC(50)), -EAGAIN);
	zassert_true(k_uptime_get() - start >= 50);

	/* Not -EBUSY of k_mutex_lock() */
	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_NO_WAIT), -EAGAIN);
	zassert_equal(ctx.calls, 0);

	k_sem_give(&m_release);

	zassert_ok(helper_join(0, K_SECONDS(1), &ret));
	zassert_ok(ret);
	zassert_equal(held.calls, 1);
	zassert_equal(g_fake.uplink_count, 1);
	assert_data_msg(0, g_payload, sizeof(g_payload));
}

static int cb_empty(zcbor_state_t *zs, void *user_data)
{
	(*(int *)user_data)++;

	return 0;
}

ZTEST(hio_cloud_send_data_cb, test_empty_nothing_sent)
{
	int calls = 0;

	zassert_equal(hio_cloud_send_data_cb(cb_empty, &calls, K_FOREVER), -EINVAL);
	zassert_equal(calls, 1);
	assert_nothing_transferred();
	assert_lock_free();
}

struct fill_ctx {
	size_t extra; /* Bytes over the exact fit */
	size_t free_before;
	size_t free_after;
};

static int cb_fill(zcbor_state_t *zs, void *user_data)
{
	struct fill_ctx *ctx = user_data;

	ctx->free_before = hio_cloud_cbor_free(zs);

	/* 3 bytes of the bstr head (0x59 + u16 length) */
	zcbor_bstr_encode_ptr(zs, g_big, ctx->free_before - 3 + ctx->extra);

	ctx->free_after = hio_cloud_cbor_free(zs);

	return 0;
}

ZTEST(hio_cloud_send_data_cb, test_exact_capacity)
{
	const size_t max = HIO_CLOUD_TRANSFER_BUF_SIZE - HDR_LEN;
	struct fill_ctx exact = {0};
	struct fill_ctx over = {.extra = 1};
	uint8_t expected[FAKE_HEAD_LEN - HDR_LEN];
	static uint8_t full[HIO_CLOUD_TRANSFER_BUF_SIZE];

	for (size_t i = 0; i < sizeof(g_big); i++) {
		g_big[i] = i;
	}

	zassert_ok(hio_cloud_send_data_cb(cb_fill, &exact, K_FOREVER));
	zassert_equal(exact.free_before, max);
	zassert_equal(exact.free_after, 0);

	full[0] = 0x59;
	sys_put_be16(max - 3, &full[1]);
	expected[0] = 0x59;
	sys_put_be16(max - 3, &expected[1]);
	memcpy(&expected[3], g_big, sizeof(expected) - 3);

	zassert_equal(g_fake.uplink_count, 1);
	assert_data_msg(0, expected, max);
	memcpy(&full[3], g_big, max - 3);
	assert_data_msg_crc(0, full, max);
	zassert_equal(g_fake.uplinks[0].len, HIO_CLOUD_TRANSFER_BUF_SIZE);
	zassert_equal(g_fake.uplinks[0].buf_size, HIO_CLOUD_TRANSFER_BUF_SIZE);

	zassert_equal(hio_cloud_send_data_cb(cb_fill, &over, K_FOREVER), -ENOSPC);
	settle();
	zassert_equal(g_fake.uplink_count, 1);
	zassert_equal(g_fake.downlink_count, 0);
}

ZTEST(hio_cloud_send_data_cb, test_flag_cleared_after_error)
{
	struct cb_ctx fail = {.ret = -EIO};
	struct cb_ctx ctx = {0};

	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &fail, K_FOREVER), -EIO);

	/* Not taken for a call from a callback */
	zassert_ok(hio_cloud_send_data(g_payload2, sizeof(g_payload2), K_FOREVER));
	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER));

	zassert_equal(g_fake.uplink_count, 2);
	assert_data_msg(0, g_payload2, sizeof(g_payload2));
	assert_data_msg(1, g_payload, sizeof(g_payload));
}

ZTEST(hio_cloud_send_data_cb, test_forever_timeout)
{
	struct cb_ctx ctx = {0};

	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER));
	zassert_equal(g_fake.uplink_count, 1);
	zassert_true(K_TIMEOUT_EQ(g_fake.uplinks[0].timeout, K_FOREVER));
}

ZTEST(hio_cloud_send_data_cb, test_msg_bytes)
{
	struct cb_ctx ctx = {0};

	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_MSEC(1234)));
	zassert_equal(ctx.calls, 1);
	zassert_equal_ptr(m_last_user_data, &ctx);

	zassert_equal(g_fake.uplink_count, 1);
	assert_data_msg(0, g_payload, sizeof(g_payload));
	zassert_equal(g_fake.uplinks[0].buf_size, HIO_CLOUD_TRANSFER_BUF_SIZE);
	zassert_equal(g_fake.uplinks[0].thread, k_current_get());
}

ZTEST(hio_cloud_send_data_cb, test_matches_send_data)
{
	struct cb_ctx ctx = {0};

	zassert_ok(hio_cloud_send_data(g_payload, sizeof(g_payload), K_FOREVER));
	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER));

	zassert_equal(g_fake.uplink_count, 2);
	zassert_equal(g_fake.uplinks[0].len, g_fake.uplinks[1].len);
	zassert_mem_equal(g_fake.uplinks[0].head, g_fake.uplinks[1].head, g_fake.uplinks[0].len);
}

ZTEST(hio_cloud_send_data_cb, test_no_wait_never_encodes)
{
	struct cb_ctx ctx = {0};

	/* The lock is free, but the deadline has passed before the callback */
	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_NO_WAIT), -ETIMEDOUT);
	zassert_equal(ctx.calls, 0);
	assert_nothing_transferred();
	assert_lock_free();
}

ZTEST(hio_cloud_send_data_cb, test_other_thread_send_data_waits)
{
	struct cb_ctx held = {.entered = true, .release = true};
	int ret0 = INT_MIN;
	int ret1 = INT_MIN;

	/* Helper 0 holds the cloud lock in its callback */
	helper_start(0, send_cb_held, &held);
	zassert_ok(k_sem_take(&m_entered, K_SECONDS(1)));

	/* Another thread is not mistaken for a nested call: it waits */
	helper_start(1, send_payload2, NULL);
	k_sleep(K_MSEC(100));
	zassert_equal(g_fake.uplink_count, 0);

	k_sem_give(&m_release);

	zassert_ok(helper_join(0, K_SECONDS(1), &ret0));
	zassert_ok(helper_join(1, K_SECONDS(1), &ret1));
	zassert_ok(ret0);
	zassert_ok(ret1);

	zassert_equal(g_fake.uplink_count, 2);
	assert_data_msg(0, g_payload, sizeof(g_payload));
	assert_data_msg(1, g_payload2, sizeof(g_payload2));
}

static int cb_overflow(zcbor_state_t *zs, void *user_data)
{
	size_t *free = user_data;

	*free = hio_cloud_cbor_free(zs);

	/* The error is ignored on purpose: the zcbor state keeps it */
	(void)hio_cloud_cbor_put_raw(zs, g_big, *free + 1);

	return 0;
}

ZTEST(hio_cloud_send_data_cb, test_overflow_nothing_sent)
{
	size_t free = 0;

	zassert_equal(hio_cloud_send_data_cb(cb_overflow, &free, K_FOREVER), -ENOSPC);
	zassert_equal(free, HIO_CLOUD_TRANSFER_BUF_SIZE - HDR_LEN);
	assert_nothing_transferred();
	assert_lock_free();
}

ZTEST(hio_cloud_send_data_cb, test_remaining_timeout_after_encode)
{
	struct cb_ctx ctx = {.sleep = K_MSEC(100)};

	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_MSEC(1000)));
	zassert_equal(g_fake.uplink_count, 1);

	assert_timeout_in_ms(0, 800, 900);
}

ZTEST(hio_cloud_send_data_cb, test_remaining_timeout_after_lock_wait)
{
	struct cb_ctx held = {.entered = true, .sleep = K_MSEC(100)};
	struct cb_ctx ctx = {0};
	int ret = INT_MIN;

	/* Helper 0 holds the cloud lock in its callback for 100 ms */
	helper_start(0, send_cb_held, &held);
	zassert_ok(k_sem_take(&m_entered, K_SECONDS(1)));

	zassert_ok(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_MSEC(1000)));

	zassert_ok(helper_join(0, K_SECONDS(1), &ret));
	zassert_ok(ret);
	zassert_equal(g_fake.uplink_count, 2);

	assert_timeout_in_ms(1, 800, 900);
}

ZTEST(hio_cloud_send_data_cb, test_transfer_error)
{
	struct cb_ctx ctx = {0};

	g_fake.uplink_ret = -EIO;

	zassert_equal(hio_cloud_send_data_cb(cb_map_1_42, &ctx, K_FOREVER), -EIO);
	zassert_equal(g_fake.uplink_count, 1);
	assert_lock_free();
}

/* ---- Cloud API called from the callback ---- */

/* Differs from g_payload in the first byte: the transfer buffer reset only
 * clears the length, so an overwrite with the same bytes would go unnoticed */
static const uint8_t m_inner[] = {0xa1, 0x05, 0x06};

struct nested_ctx {
	int (*nested)(void);
	int nested_ret;
	size_t len_before;
	size_t len_after;
};

/* Encodes {1: 42} with the nested call between the key and the value */
static int cb_outer(zcbor_state_t *zs, void *user_data)
{
	struct nested_ctx *ctx = user_data;

	zcbor_map_start_encode(zs, 1);
	zcbor_uint32_put(zs, 1);

	ctx->len_before = hio_cloud_cbor_len(zs);
	ctx->nested_ret = ctx->nested();
	ctx->len_after = hio_cloud_cbor_len(zs);

	zcbor_uint32_put(zs, 42);
	zcbor_map_end_encode(zs, 1);

	return 0;
}

static void run_nested(int (*nested)(void))
{
	struct nested_ctx ctx = {.nested = nested, .nested_ret = INT_MIN};

	zassert_ok(hio_cloud_send_data_cb(cb_outer, &ctx, K_FOREVER));
	zassert_equal(ctx.nested_ret, -EDEADLK);

	/* The outer message is intact */
	zassert_equal(ctx.len_before, 2);
	zassert_equal(ctx.len_after, ctx.len_before);

	settle();
	zassert_equal(g_fake.uplink_count, 1);
	zassert_equal(g_fake.downlink_count, 0);
	assert_data_msg(0, g_payload, sizeof(g_payload));
	assert_data_msg_crc(0, g_payload, sizeof(g_payload));

	assert_lock_free();
}

ZTEST_SUITE(hio_cloud_send_data_cb_nested, phase_started, NULL, before, NULL, NULL);

static int nested_firmware_update(void)
{
	return hio_cloud_firmware_update("fw");
}

ZTEST(hio_cloud_send_data_cb_nested, test_nested_firmware_update)
{
	run_nested(nested_firmware_update);
}

static int nested_recv(void)
{
	return hio_cloud_recv();
}

ZTEST(hio_cloud_send_data_cb_nested, test_nested_recv)
{
	run_nested(nested_recv);
}

static int nested_send(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	return hio_cloud_send(m_inner, sizeof(m_inner));
#pragma GCC diagnostic pop
}

ZTEST(hio_cloud_send_data_cb_nested, test_nested_send)
{
	run_nested(nested_send);
}

static int nested_send_data(void)
{
	return hio_cloud_send_data(m_inner, sizeof(m_inner), K_FOREVER);
}

ZTEST(hio_cloud_send_data_cb_nested, test_nested_send_data)
{
	run_nested(nested_send_data);
}

static int cb_inner(zcbor_state_t *zs, void *user_data)
{
	m_inner_calls++;

	return zcbor_uint32_put(zs, 7) ? 0 : -EIO;
}

static int nested_send_data_cb(void)
{
	return hio_cloud_send_data_cb(cb_inner, NULL, K_FOREVER);
}

ZTEST(hio_cloud_send_data_cb_nested, test_nested_send_data_cb)
{
	run_nested(nested_send_data_cb);
	zassert_equal(m_inner_calls, 0);
}
