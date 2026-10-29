/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "test_module.h"

#include <zephyr/ztest.h>

#include <string.h>

/* SETTINGS_NONE has no save destination (settings_save() returns -ENOENT),
 * so register a store that accepts and drops every value. */
static int null_save(struct settings_store *cs, const char *name, const char *value,
		     size_t val_len)
{
	return 0;
}

static const struct settings_store_itf m_null_itf = {
	.csi_save = null_save,
};

static struct settings_store m_null_store = {
	.cs_itf = &m_null_itf,
};

/* Suites run in alphabetical order; register here too (idempotent). */
static void *suite_setup(void)
{
	zassert_ok(test_module_register(), "test module registration failed");
	settings_dst_register(&m_null_store);
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
