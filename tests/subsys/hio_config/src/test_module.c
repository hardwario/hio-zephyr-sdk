/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "test_module.h"

#include <string.h>

static const char *m_mode_labels[] = {"off", "slow", "fast"};
static const uint8_t m_key_default[8] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03};

struct test_config g_test_config_interim;
static struct test_config m_config_final;

static struct hio_config_item m_items[] = {
	HIO_CONFIG_ITEM_INT("interval", g_test_config_interim.interval, 1, 86400,
			    "Sampling interval", 60),
	HIO_CONFIG_ITEM_FLOAT("threshold", g_test_config_interim.threshold, 0.f, 100.f,
			      "Alert threshold", 1.5f),
	HIO_CONFIG_ITEM_BOOL("enabled", g_test_config_interim.enabled, "Feature enable", true),
	HIO_CONFIG_ITEM_ENUM("mode", g_test_config_interim.mode, m_mode_labels, "Operating mode",
			     TEST_MODE_SLOW),
	HIO_CONFIG_ITEM_STRING("apn", g_test_config_interim.apn, "Network APN", "internet"),
	HIO_CONFIG_ITEM_HEX("key", g_test_config_interim.key, "Encryption key", m_key_default),
	HIO_CONFIG_ITEM_STRING("secret", g_test_config_interim.secret, "Hidden secret", "s3cret"),
	HIO_CONFIG_ITEM_INT("rovalue", g_test_config_interim.rovalue, 0, 100, "Read-only value",
			    42),
};

struct hio_config g_test_module = {
	.name = "app",
	.items = m_items,
	.nitems = ARRAY_SIZE(m_items),
	.interim = &g_test_config_interim,
	.final = &m_config_final,
	.size = sizeof(struct test_config),
};

static int access_cb(const struct hio_config *module, const struct hio_config_item *item)
{
	if (strcmp(item->name, "secret") == 0) {
		return HIO_CONFIG_ACCESS_HIDDEN_RW;
	}
	if (strcmp(item->name, "rovalue") == 0) {
		return HIO_CONFIG_ACCESS_RO;
	}
	return HIO_CONFIG_ACCESS_RW;
}

int test_module_register(void)
{
	hio_config_set_access_cb(access_cb);

	/* Ztest runs suites in alphabetical order (iterable sections are
	 * SORT_BY_NAME), so every suite's setup calls this; tolerate repeats. */
	int ret = hio_config_register(&g_test_module);
	return ret == -EALREADY ? 0 : ret;
}

void test_module_set_defaults(void)
{
	g_test_config_interim.interval = 60;
	g_test_config_interim.threshold = 1.5f;
	g_test_config_interim.enabled = true;
	g_test_config_interim.mode = TEST_MODE_SLOW;
	strcpy(g_test_config_interim.apn, "internet");
	memcpy(g_test_config_interim.key, m_key_default, sizeof(m_key_default));
	strcpy(g_test_config_interim.secret, "s3cret");
	g_test_config_interim.rovalue = 42;
}
