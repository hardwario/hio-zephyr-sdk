/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "fake_backend.h"

/* HIO includes */
#include <hio/hio_cloud.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

static struct hio_cloud_options m_options = {
	.decoder_hash = HASH,
	.encoder_hash = ENCODER_HASH,
};

/* hio_cloud_init() cannot be undone: the suites for the uninitialized subsystem
 * run first, then the subsystem is initialized over the fake transport and the
 * rest runs. If the init fails, the second run is empty and the check that every
 * suite ran fails the test. Suites and tests run in alphabetical order. */
void test_main(void)
{
	struct test_state state = {.started = false};

	ztest_run_all(&state, false, 1, 1);

	fake_reset();

	/* Before the init, which confirms a firmware update */
	mock_fw_setup();

	int ret = hio_cloud_init(&m_options);

	if (!ret) {
		ret = hio_cloud_wait_initialized(TEST_INIT_TIMEOUT);
	}

	if (ret) {
		printk("hio_cloud init failed: %d\n", ret);
	}

	state.started = !ret;

	ztest_run_all(&state, false, 1, 1);

	ztest_verify_all_test_suites_ran();
}
