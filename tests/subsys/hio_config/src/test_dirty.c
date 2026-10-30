/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "ram_store.h"
#include "test_module.h"

#include <zephyr/ztest.h>

#include <string.h>

/* Suites run in alphabetical order; register here too (idempotent). */
static void *suite_setup(void)
{
	zassert_ok(test_module_register(), "test module registration failed");
	ram_store_register();
	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_module_set_defaults();
	zassert_ok(hio_config_save_without_reboot());
}

ZTEST_SUITE(hio_config_dirty, NULL, suite_setup, before_each, NULL, NULL);

static int set(const char *name, const char *value, const char **err_msg)
{
	const struct hio_config_item *item;
	char buf[64];

	zassert_ok(hio_config_module_find_item(&g_test_module, name, &item));
	strcpy(buf, value);
	return hio_config_module_item_set_value(&g_test_module, item, buf, err_msg);
}

ZTEST(hio_config_dirty, test_clean_after_save)
{
	zassert_false(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_set_marks_dirty)
{
	const char *err_msg = NULL;

	zassert_ok(set("interval", "120", &err_msg));
	zassert_true(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_same_value_marks_dirty)
{
	const char *err_msg = NULL;

	zassert_ok(set("interval", "60", &err_msg));
	zassert_true(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_failed_set_keeps_clean)
{
	const char *err_msg = NULL;

	zassert_not_ok(set("interval", "0", &err_msg));
	zassert_not_ok(set("rovalue", "50", &err_msg));
	zassert_false(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_parse_line_marks_dirty)
{
	char line[] = "app config apn \"example\"";

	zassert_ok(hio_config_parse_line(line, NULL));
	zassert_true(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_save_clears)
{
	const char *err_msg = NULL;

	zassert_ok(set("enabled", "false", &err_msg));
	zassert_true(hio_config_is_dirty());
	zassert_ok(hio_config_save_without_reboot());
	zassert_false(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_gen_counts_writes)
{
	const char *err_msg = NULL;
	char line[] = "app config apn \"example\"";
	uint32_t gen = hio_config_get_gen();

	zassert_ok(set("interval", "120", &err_msg));
	zassert_equal(hio_config_get_gen(), gen + 1);
	zassert_ok(set("interval", "120", &err_msg));
	zassert_equal(hio_config_get_gen(), gen + 2);
	zassert_not_ok(set("interval", "0", &err_msg));
	zassert_equal(hio_config_get_gen(), gen + 2);
	zassert_ok(hio_config_parse_line(line, NULL));
	zassert_equal(hio_config_get_gen(), gen + 3);
}

ZTEST(hio_config_dirty, test_save_keeps_gen)
{
	const char *err_msg = NULL;

	zassert_ok(set("interval", "120", &err_msg));
	uint32_t gen = hio_config_get_gen();

	zassert_ok(hio_config_save_without_reboot());
	zassert_equal(hio_config_get_gen(), gen);
	zassert_false(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_reset_marks_dirty)
{
	uint32_t gen = hio_config_get_gen();

	zassert_ok(hio_config_reset_without_reboot());
	zassert_equal(hio_config_get_gen(), gen + 1);
	zassert_true(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_module_reset_keeps_clean)
{
	uint32_t gen = hio_config_get_gen();

	zassert_ok(hio_config_module_reset_without_reboot(&g_test_module));
	zassert_equal(hio_config_get_gen(), gen + 1);
	zassert_false(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_module_reset_keeps_dirty)
{
	const char *err_msg = NULL;

	zassert_ok(set("interval", "120", &err_msg));
	zassert_ok(hio_config_module_reset_without_reboot(&g_test_module));
	zassert_true(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_store_keeps_clean)
{
	const struct hio_config_item *item;
	uint32_t gen = hio_config_get_gen();
	int value = 300;

	zassert_ok(hio_config_module_find_item(&g_test_module, "interval", &item));
	zassert_ok(hio_config_module_item_store(&g_test_module, item, &value));
	zassert_equal(g_test_config_interim.interval, 300);
	zassert_equal(hio_config_get_gen(), gen + 1);
	zassert_false(hio_config_is_dirty());
}

ZTEST(hio_config_dirty, test_store_keeps_dirty)
{
	const struct hio_config_item *item;
	const char *err_msg = NULL;
	int value = 300;

	zassert_ok(set("apn", "other", &err_msg));
	zassert_ok(hio_config_module_find_item(&g_test_module, "interval", &item));
	zassert_ok(hio_config_module_item_store(&g_test_module, item, &value));
	zassert_true(hio_config_is_dirty());
}
