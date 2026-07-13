/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "test_module.h"

#include <zephyr/ztest.h>

#include <string.h>

/* Suites run in alphabetical order, so each suite registers the module
 * itself (test_module_register is idempotent). */
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

ZTEST_SUITE(hio_config_format_line, NULL, suite_setup, before_each, NULL, NULL);

static const char *format_item(const char *name, char *buf, size_t size)
{
	const struct hio_config_item *item;
	int ret;

	zassert_ok(hio_config_module_find_item(&g_test_module, name, &item));
	ret = hio_config_item_format_line(&g_test_module, item, buf, size);
	zassert_true(ret > 0, "format_line failed: %d", ret);
	zassert_equal(ret, strlen(buf));
	return buf;
}

ZTEST(hio_config_format_line, test_int)
{
	char buf[128];
	zassert_str_equal(format_item("interval", buf, sizeof(buf)),
			  "app config interval 60");
}

ZTEST(hio_config_format_line, test_float)
{
	char buf[128];
	zassert_str_equal(format_item("threshold", buf, sizeof(buf)),
			  "app config threshold 1.50");
}

ZTEST(hio_config_format_line, test_bool)
{
	char buf[128];
	zassert_str_equal(format_item("enabled", buf, sizeof(buf)),
			  "app config enabled true");
}

ZTEST(hio_config_format_line, test_enum)
{
	char buf[128];
	zassert_str_equal(format_item("mode", buf, sizeof(buf)),
			  "app config mode \"slow\"");
}

ZTEST(hio_config_format_line, test_enum_invalid)
{
	char buf[128];
	g_test_config_interim.mode = (enum test_mode)7;
	zassert_str_equal(format_item("mode", buf, sizeof(buf)),
			  "app config mode <invalid: 7>");
}

ZTEST(hio_config_format_line, test_string)
{
	char buf[128];
	zassert_str_equal(format_item("apn", buf, sizeof(buf)),
			  "app config apn \"internet\"");
}

ZTEST(hio_config_format_line, test_hex)
{
	char buf[128];
	zassert_str_equal(format_item("key", buf, sizeof(buf)),
			  "app config key deadbeef00010203");
}

ZTEST(hio_config_format_line, test_enospc)
{
	char buf[8];
	const struct hio_config_item *item;

	zassert_ok(hio_config_module_find_item(&g_test_module, "interval", &item));
	zassert_equal(hio_config_item_format_line(&g_test_module, item, buf, sizeof(buf)),
		      -ENOSPC);
}
