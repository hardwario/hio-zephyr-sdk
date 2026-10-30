/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "ram_store.h"
#include "test_module.h"

#include <zephyr/ztest.h>

#include <string.h>

struct seen {
	int count;
	char names[8][16];
	char lines[8][128];
};

static int collect_cb(const struct hio_config *module, const struct hio_config_item *item,
		      const void *value, void *user_data)
{
	struct seen *seen = user_data;

	zassert_true(seen->count < ARRAY_SIZE(seen->names));
	strcpy(seen->names[seen->count], item->name);
	zassert_true(hio_config_item_format_line_value(module, item, value,
						       seen->lines[seen->count],
						       sizeof(seen->lines[0])) > 0);
	seen->count++;

	return 0;
}

static const char *find_line(const struct seen *seen, const char *name)
{
	for (int i = 0; i < seen->count; i++) {
		if (strcmp(seen->names[i], name) == 0) {
			return seen->lines[i];
		}
	}

	return NULL;
}

static int set(const char *name, const char *value)
{
	const struct hio_config_item *item;
	const char *err_msg = NULL;
	char buf[64];

	zassert_ok(hio_config_module_find_item(&g_test_module, name, &item));
	strcpy(buf, value);
	return hio_config_module_item_set_value(&g_test_module, item, buf, &err_msg);
}

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
	ram_store_clear();
	test_module_set_defaults();
	zassert_ok(hio_config_save_without_reboot());
}

ZTEST_SUITE(hio_config_saved, NULL, suite_setup, before_each, NULL, NULL);

ZTEST(hio_config_saved, test_format_line_value)
{
	const struct hio_config_item *item;
	char line[64];
	int value = 7;

	zassert_ok(hio_config_module_find_item(&g_test_module, "interval", &item));
	zassert_true(hio_config_item_format_line_value(&g_test_module, item, &value, line,
						       sizeof(line)) > 0);
	zassert_str_equal(line, "app config interval 7");
}

ZTEST(hio_config_saved, test_saved_equals_current_after_save)
{
	struct seen seen = {0};

	zassert_ok(set("interval", "120"));
	zassert_ok(set("apn", "other"));
	zassert_ok(hio_config_save_without_reboot());
	zassert_ok(hio_config_iter_saved(collect_cb, &seen));

	/* All shown items, hidden one excluded */
	zassert_equal(seen.count, 7);
	zassert_is_null(find_line(&seen, "secret"));
	zassert_str_equal(find_line(&seen, "interval"), "app config interval 120");
	zassert_str_equal(find_line(&seen, "apn"), "app config apn \"other\"");
}

ZTEST(hio_config_saved, test_saved_keeps_old_values)
{
	struct seen seen = {0};

	zassert_ok(set("interval", "120"));
	zassert_ok(hio_config_iter_saved(collect_cb, &seen));
	zassert_str_equal(find_line(&seen, "interval"), "app config interval 60");
}

ZTEST(hio_config_saved, test_saved_missing_uses_default)
{
	struct seen seen = {0};

	ram_store_clear();
	zassert_ok(hio_config_iter_saved(collect_cb, &seen));
	zassert_equal(seen.count, 7);
	zassert_str_equal(find_line(&seen, "apn"), "app config apn \"internet\"");
	zassert_str_equal(find_line(&seen, "rovalue"), "app config rovalue 42");
}

ZTEST(hio_config_saved, test_saved_size_mismatch_uses_default)
{
	struct seen seen = {0};
	const uint16_t bad = 5;

	ram_store_put("app/interval", &bad, sizeof(bad));
	zassert_ok(hio_config_iter_saved(collect_cb, &seen));
	zassert_str_equal(find_line(&seen, "interval"), "app config interval 60");
}

ZTEST(hio_config_saved, test_diff_empty_when_clean)
{
	struct seen seen = {0};

	/* A clean config must not even read the storage */
	ram_store_clear();
	ram_store_put("app/interval", &(int){999}, sizeof(int));
	zassert_ok(hio_config_iter_diff(collect_cb, &seen));
	zassert_equal(seen.count, 0);
}

ZTEST(hio_config_saved, test_diff_reports_changes)
{
	struct seen seen = {0};

	zassert_ok(set("interval", "120"));
	zassert_ok(set("apn", "other"));
	zassert_ok(set("enabled", "true"));
	zassert_ok(hio_config_iter_diff(collect_cb, &seen));

	zassert_equal(seen.count, 2);
	zassert_str_equal(find_line(&seen, "interval"), "app config interval 60");
	zassert_str_equal(find_line(&seen, "apn"), "app config apn \"internet\"");
}

ZTEST(hio_config_saved, test_diff_skips_reverted_and_hidden)
{
	struct seen seen = {0};

	zassert_ok(set("interval", "120"));
	zassert_ok(set("interval", "60"));
	zassert_ok(set("secret", "changed"));
	zassert_true(hio_config_is_dirty());
	zassert_ok(hio_config_iter_diff(collect_cb, &seen));
	zassert_equal(seen.count, 0);
}

ZTEST(hio_config_saved, test_diff_string_ignores_bytes_after_nul)
{
	struct seen seen = {0};
	char stored[sizeof(g_test_config_interim.apn)];

	/* Same string, different padding after the terminator */
	memset(stored, 'x', sizeof(stored));
	strcpy(stored, "internet");
	ram_store_put("app/apn", stored, sizeof(stored));

	zassert_ok(set("interval", "60"));
	zassert_ok(hio_config_iter_diff(collect_cb, &seen));
	zassert_equal(seen.count, 0);
}

ZTEST(hio_config_saved, test_store_is_saved)
{
	const struct hio_config_item *item;
	struct seen seen = {0};
	int value = 300;

	zassert_ok(set("apn", "other"));
	zassert_ok(hio_config_module_find_item(&g_test_module, "interval", &item));
	zassert_ok(hio_config_module_item_store(&g_test_module, item, &value));

	/* Only the unsaved APN differs, the stored interval does not */
	zassert_ok(hio_config_iter_diff(collect_cb, &seen));
	zassert_equal(seen.count, 1);
	zassert_not_null(find_line(&seen, "apn"));
}
