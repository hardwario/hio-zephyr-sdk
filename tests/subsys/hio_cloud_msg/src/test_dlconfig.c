/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_msg.h"
#include "test_module.h"

#include <hio/hio_buf.h>
#include <hio/hio_config.h>

#include <zephyr/ztest.h>

#include <zcbor_common.h>
#include <zcbor_encode.h>

#include <string.h>

/* Suites run in alphabetical order and this one runs first in the binary;
 * register the module here (test_module_register is idempotent). */
static void *suite_setup(void)
{
	zassert_ok(test_module_register(), "test module registration failed");
	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_module_set_defaults();
}

ZTEST_SUITE(hio_cloud_dlconfig, NULL, suite_setup, before_each, NULL, NULL);

/* Build a DL_DOWNLOAD_CONFIG message: [0x82][0x00][CBOR list of tstr]. */
static void build_dlconfig(struct hio_buf *buf, const char **lines, int count)
{
	hio_buf_reset(buf);
	zassert_ok(hio_buf_append_u8(buf, DL_DOWNLOAD_CONFIG));
	zassert_ok(hio_buf_append_u8(buf, 0x00));

	uint8_t *p = hio_buf_get_mem(buf) + 2;

	ZCBOR_STATE_E(zs, 0, p, hio_buf_get_free(buf), 1);
	zassert_true(zcbor_list_start_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH));
	for (int i = 0; i < count; i++) {
		zassert_true(zcbor_tstr_put_term(zs, lines[i], strlen(lines[i])));
	}
	zassert_true(zcbor_list_end_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH));

	zassert_ok(hio_buf_seek(buf, 2 + (zs->payload - p)));
}

static int apply_dlconfig(struct hio_buf *buf)
{
	struct hio_cloud_msg_dlconfig config;

	int ret = hio_cloud_msg_unpack_config(buf, &config);
	if (ret) {
		return ret;
	}

	HIO_BUF_DEFINE(line, 512);

	/* Mirrors the production loop in hio_cloud_process_dlconfig: count
	 * applied (non-empty) lines, not consumed CBOR entries — get_next_line
	 * consumes entries sequentially including empty ones. */
	for (int i = 0; i < config.lines;) {
		hio_buf_reset(&line);

		ret = hio_cloud_msg_dlconfig_get_next_line(&config, &line);
		if (ret) {
			return ret;
		}

		char *cmd = (char *)hio_buf_get_mem(&line);

		if (cmd[0] == '\0') {
			continue; /* does not count toward config.lines */
		}

		const char *err_msg = NULL;

		ret = hio_config_parse_line(cmd, &err_msg);
		if (ret) {
			return ret;
		}

		i++;
	}

	return 0;
}

ZTEST(hio_cloud_dlconfig, test_apply_lines_all_types)
{
	static const char *lines[] = {
		"app config interval 300",    "app config threshold 9.75",
		"app config enabled false",   "app config mode \"fast\"",
		"app config apn \"new apn\"", "app config key 0102030405060708",
	};

	HIO_BUF_DEFINE(buf, 1024);
	build_dlconfig(&buf, lines, ARRAY_SIZE(lines));

	zassert_ok(apply_dlconfig(&buf));

	static const uint8_t expected_key[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

	zassert_equal(g_test_config_interim.interval, 300);
	zassert_within(g_test_config_interim.threshold, 9.75f, 0.001f);
	zassert_false(g_test_config_interim.enabled);
	zassert_equal(g_test_config_interim.mode, TEST_MODE_FAST);
	zassert_str_equal(g_test_config_interim.apn, "new apn");
	zassert_mem_equal(g_test_config_interim.key, expected_key, 8);
}

ZTEST(hio_cloud_dlconfig, test_first_error_aborts)
{
	static const char *lines[] = {
		"app config interval 300",
		"app config nonexistent 1",
		"app config enabled false",
	};

	HIO_BUF_DEFINE(buf, 1024);
	build_dlconfig(&buf, lines, ARRAY_SIZE(lines));

	zassert_true(apply_dlconfig(&buf) < 0);

	/* First line applied, third line NOT applied (abort after error). */
	zassert_equal(g_test_config_interim.interval, 300);
	zassert_true(g_test_config_interim.enabled);
}

ZTEST(hio_cloud_dlconfig, test_empty_line_mid_stream)
{
	/* build_dlconfig's count feeds 3 zcbor entries, but unpack_config
	 * only counts the 2 non-empty ones into config.lines. The line
	 * after the empty entry must still be consumed and applied. */
	static const char *lines[] = {
		"app config interval 300",
		"",
		"app config enabled false",
	};

	HIO_BUF_DEFINE(buf, 1024);
	build_dlconfig(&buf, lines, ARRAY_SIZE(lines));

	zassert_ok(apply_dlconfig(&buf));

	zassert_equal(g_test_config_interim.interval, 300);
	zassert_false(g_test_config_interim.enabled);
}

ZTEST(hio_cloud_dlconfig, test_read_only_rejected)
{
	static const char *lines[] = {"app config rovalue 7"};

	HIO_BUF_DEFINE(buf, 1024);
	build_dlconfig(&buf, lines, ARRAY_SIZE(lines));

	zassert_equal(apply_dlconfig(&buf), -EPERM);
	zassert_equal(g_test_config_interim.rovalue, 42);
}
