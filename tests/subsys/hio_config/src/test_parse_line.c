/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "test_module.h"

#include <zephyr/ztest.h>

#include <string.h>

/* Suites run in alphabetical order; register here too (idempotent). */
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

ZTEST_SUITE(hio_config_parse_line, NULL, suite_setup, before_each, NULL, NULL);

static int parse(const char *input, const char **err_msg)
{
	char line[128];

	strcpy(line, input);
	return hio_config_parse_line(line, err_msg);
}

ZTEST(hio_config_parse_line, test_int)
{
	zassert_ok(parse("app config interval 120", NULL));
	zassert_equal(g_test_config_interim.interval, 120);
}

ZTEST(hio_config_parse_line, test_float)
{
	zassert_ok(parse("app config threshold 2.25", NULL));
	zassert_within(g_test_config_interim.threshold, 2.25f, 0.001f);
}

ZTEST(hio_config_parse_line, test_bool)
{
	zassert_ok(parse("app config enabled false", NULL));
	zassert_false(g_test_config_interim.enabled);
}

ZTEST(hio_config_parse_line, test_enum_quoted)
{
	zassert_ok(parse("app config mode \"fast\"", NULL));
	zassert_equal(g_test_config_interim.mode, TEST_MODE_FAST);
}

ZTEST(hio_config_parse_line, test_string_quoted_with_space)
{
	zassert_ok(parse("app config apn \"my apn\"", NULL));
	zassert_str_equal(g_test_config_interim.apn, "my apn");
}

ZTEST(hio_config_parse_line, test_string_empty)
{
	zassert_ok(parse("app config apn \"\"", NULL));
	zassert_str_equal(g_test_config_interim.apn, "");
}

ZTEST(hio_config_parse_line, test_hex_unquoted)
{
	static const uint8_t expected[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

	zassert_ok(parse("app config key 0102030405060708", NULL));
	zassert_mem_equal(g_test_config_interim.key, expected, sizeof(expected));
}

ZTEST(hio_config_parse_line, test_roundtrip_all_visible_items)
{
	for (int i = 0; i < g_test_module.nitems; i++) {
		const struct hio_config_item *item = &g_test_module.items[i];

		if (!(hio_config_item_access(&g_test_module, item) & HIO_CONFIG_ACCESS_WRITE) ||
		    !(hio_config_item_access(&g_test_module, item) & HIO_CONFIG_ACCESS_SHOW)) {
			continue;
		}

		char line[128];
		char copy[128];
		const char *err_msg = NULL;

		int len = hio_config_item_format_line(&g_test_module, item, line, sizeof(line));
		zassert_true(len > 0);
		strcpy(copy, line);

		zassert_ok(hio_config_parse_line(copy, &err_msg), "roundtrip failed for: %s (%s)",
			   line, err_msg ? err_msg : "");

		char line2[128];
		zassert_true(hio_config_item_format_line(&g_test_module, item, line2,
							 sizeof(line2)) > 0);
		zassert_str_equal(line, line2, "value changed by roundtrip");
	}
}

ZTEST(hio_config_parse_line, test_unknown_module)
{
	const char *err_msg = NULL;
	zassert_true(parse("nope config interval 1", &err_msg) < 0);
}

ZTEST(hio_config_parse_line, test_unknown_item)
{
	const char *err_msg = NULL;
	zassert_true(parse("app config nope 1", &err_msg) < 0);
}

ZTEST(hio_config_parse_line, test_malformed_missing_keyword)
{
	const char *err_msg = NULL;
	zassert_equal(parse("app interval 1", &err_msg), -EINVAL);
	zassert_not_null(err_msg);
}

ZTEST(hio_config_parse_line, test_malformed_missing_value)
{
	const char *err_msg = NULL;
	zassert_equal(parse("app config interval", &err_msg), -EINVAL);
}

ZTEST(hio_config_parse_line, test_out_of_range)
{
	const char *err_msg = NULL;
	zassert_true(parse("app config interval 999999", &err_msg) < 0);
	zassert_equal(g_test_config_interim.interval, 60, "value must not change on error");
}

ZTEST(hio_config_parse_line, test_read_only_item)
{
	const char *err_msg = NULL;
	zassert_equal(parse("app config rovalue 7", &err_msg), -EPERM);
	zassert_equal(g_test_config_interim.rovalue, 42);
}
