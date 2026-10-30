/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_msg.h"
#include "ram_store.h"
#include "test_module.h"

#include <hio/hio_buf.h>
#include <hio/hio_config.h>

#include <zephyr/ztest.h>

#include <string.h>

/* Suites run in alphabetical order, so this one runs before
 * hio_cloud_pack_config registers its "big" module with items larger than
 * CONFIG_HIO_CONFIG_VALUE_MAX_SIZE. */
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

ZTEST_SUITE(hio_cloud_config_hash, NULL, suite_setup, before_each, NULL, NULL);

static int set(const char *name, const char *value)
{
	const struct hio_config_item *item;
	const char *err_msg = NULL;
	char buf[64];

	zassert_ok(hio_config_module_find_item(&g_test_module, name, &item));
	strcpy(buf, value);
	return hio_config_module_item_set_value(&g_test_module, item, buf, &err_msg);
}

static uint64_t upload_hash(void)
{
	HIO_BUF_DEFINE(buf, 4096);
	uint64_t hash;

	zassert_ok(hio_cloud_msg_pack_config(&buf));
	zassert_ok(hio_cloud_msg_get_hash(&buf, &hash));

	return hash;
}

ZTEST(hio_cloud_config_hash, test_hash_matches_upload)
{
	uint64_t hash;

	zassert_ok(hio_cloud_msg_config_hash(&hash));
	zassert_equal(hash, upload_hash());

	zassert_ok(set("apn", "other"));
	zassert_ok(hio_cloud_msg_config_hash(&hash));
	zassert_equal(hash, upload_hash());
}

ZTEST(hio_cloud_config_hash, test_upload_skips_unsaved)
{
	uint64_t before = upload_hash();
	uint64_t hash;

	/* Unsaved change: neither the upload nor the hash moves */
	zassert_ok(set("interval", "120"));
	zassert_equal(upload_hash(), before);
	zassert_ok(hio_cloud_msg_config_hash(&hash));
	zassert_equal(hash, before);

	/* After the save both carry the new value */
	zassert_ok(hio_config_save_without_reboot());
	zassert_ok(hio_cloud_msg_config_hash(&hash));
	zassert_not_equal(hash, before);
	zassert_equal(hash, upload_hash());
}
