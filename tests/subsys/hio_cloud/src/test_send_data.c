/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Characterization of the buffer API (hio_cloud_send_data). Uses nothing but
 * the buffer API, so the same file also runs against the hio_cloud.c of the
 * commit before the callback API (byte-identical messages and return codes). */

#include "fake_backend.h"

/* HIO includes */
#include <hio/hio_cloud.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <string.h>

/* ---- Before hio_cloud_init() ---- */

ZTEST_SUITE(hio_cloud_send_data_before_init, phase_before_init, NULL, fake_before, NULL, NULL);

ZTEST(hio_cloud_send_data_before_init, test_einval_before_init)
{
	/* Arguments are checked first */
	zassert_equal(hio_cloud_send_data(NULL, 1, K_FOREVER), -EINVAL);
	zassert_equal(hio_cloud_send_data(g_payload, 0, K_FOREVER), -EINVAL);
}

ZTEST(hio_cloud_send_data_before_init, test_eperm_before_init)
{
	zassert_equal(hio_cloud_send_data(g_payload, sizeof(g_payload), K_FOREVER), -EPERM);
	zassert_equal(g_fake.uplink_count, 0);
}

/* Runs after the *_before_init suites (alphabetical order) */
ZTEST_SUITE(hio_cloud_send_data_failed_init, phase_before_init, NULL, fake_before, NULL, NULL);

ZTEST(hio_cloud_send_data_failed_init, test_eperm_after_failed_init)
{
	static struct hio_cloud_options options = {.decoder_hash = HASH};

	g_fake.init_ret = -EIO;

	zassert_equal(hio_cloud_init(&options), -EIO);
	zassert_equal(hio_cloud_send_data(g_payload, sizeof(g_payload), K_FOREVER), -EPERM);
	zassert_equal(g_fake.uplink_count, 0);
}

/* ---- After hio_cloud_init() ---- */

ZTEST_SUITE(hio_cloud_send_data, phase_started, NULL, fake_before, NULL, NULL);

ZTEST(hio_cloud_send_data, test_capacity)
{
	const size_t max = HIO_CLOUD_TRANSFER_BUF_SIZE - HDR_LEN;

	for (size_t i = 0; i < sizeof(g_big); i++) {
		g_big[i] = i;
	}

	zassert_ok(hio_cloud_send_data(g_big, max, K_FOREVER));
	zassert_equal(g_fake.uplink_count, 1);
	assert_data_msg(0, g_big, max);
	assert_data_msg_crc(0, g_big, max);
	zassert_equal(g_fake.uplinks[0].len, HIO_CLOUD_TRANSFER_BUF_SIZE);

	zassert_equal(hio_cloud_send_data(g_big, max + 1, K_FOREVER), -ENOSPC);
	settle();
	zassert_equal(g_fake.uplink_count, 1);
	zassert_equal(g_fake.downlink_count, 0);
	assert_lock_free();
}

ZTEST(hio_cloud_send_data, test_downlink_deferred)
{
	g_fake.uplink_has_downlink = true;

	zassert_ok(hio_cloud_send_data(g_payload, sizeof(g_payload), K_FOREVER));
	zassert_equal(g_fake.uplink_count, 1);

	/* Fetched by the poll worker, not on the caller's stack */
	zassert_ok(fake_wait_downlink(K_SECONDS(1)));
	zassert_equal(g_fake.downlink_count, 1);
	zassert_not_equal(g_fake.downlink_thread, k_current_get());
}

ZTEST(hio_cloud_send_data, test_einval)
{
	zassert_equal(hio_cloud_send_data(NULL, 1, K_FOREVER), -EINVAL);
	zassert_equal(hio_cloud_send_data(g_payload, 0, K_FOREVER), -EINVAL);
	settle();
	zassert_equal(g_fake.uplink_count, 0);
	zassert_equal(g_fake.downlink_count, 0);
}

ZTEST(hio_cloud_send_data, test_msg_bytes)
{
	zassert_ok(hio_cloud_send_data(g_payload, sizeof(g_payload), K_MSEC(1234)));

	zassert_equal(g_fake.uplink_count, 1);
	assert_data_msg(0, g_payload, sizeof(g_payload));
	zassert_equal(g_fake.uplinks[0].buf_size, HIO_CLOUD_TRANSFER_BUF_SIZE);
	zassert_equal(g_fake.uplinks[0].thread, k_current_get());

	/* The timeout goes to the transfer unchanged */
	zassert_equal(g_fake.uplinks[0].timeout.ticks, K_MSEC(1234).ticks);
}

static int send_payload(void *arg)
{
	return hio_cloud_send_data(g_payload, sizeof(g_payload), K_MSEC(10));
}

ZTEST(hio_cloud_send_data, test_lock_waits_forever)
{
	int ret1 = INT_MIN;
	int ret2 = INT_MIN;

	g_fake.uplink_block = 1;

	/* Helper 0 holds the cloud lock in the transfer */
	helper_start(0, send_payload, NULL);
	zassert_ok(fake_wait_uplink_blocked(K_SECONDS(1)));

	/* Helper 1 waits for the lock longer than its timeout */
	helper_start(1, send_payload2, NULL);
	k_sleep(K_MSEC(100));
	zassert_equal(g_fake.uplink_count, 1);

	fake_release();

	zassert_ok(helper_join(0, K_SECONDS(1), &ret1));
	zassert_ok(helper_join(1, K_SECONDS(1), &ret2));
	zassert_ok(ret1);
	zassert_ok(ret2);

	zassert_equal(g_fake.uplink_count, 2);
	assert_data_msg(0, g_payload, sizeof(g_payload));
	assert_data_msg(1, g_payload2, sizeof(g_payload2));

	/* The timeout applies to the transfer only, not to the lock wait */
	zassert_equal(g_fake.uplinks[1].timeout.ticks, K_MSEC(10).ticks);
}

ZTEST(hio_cloud_send_data, test_transfer_error)
{
	g_fake.uplink_ret = -EIO;

	zassert_equal(hio_cloud_send_data(g_payload, sizeof(g_payload), K_FOREVER), -EIO);
	zassert_equal(g_fake.uplink_count, 1);
	assert_lock_free();
}
