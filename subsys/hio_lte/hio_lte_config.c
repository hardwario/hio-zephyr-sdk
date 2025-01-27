/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_lte_config.h"

/* HIO includes */
#include <hio/hio_config.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_lte_config, CONFIG_HIO_LTE_LOG_LEVEL);

#define SETTINGS_PFX "lte"

struct hio_lte_config g_hio_lte_config;
static struct hio_lte_config m_config_interim;

static const char *m_enum_auth_items[] = {"none", "pap", "chap"};

static struct hio_config_item m_config_items[] = {
	HIO_CONFIG_ITEM_BOOL("test", m_config_interim.test, "LTE test", false),
	HIO_CONFIG_ITEM_BOOL("nb-iot-mode", m_config_interim.nb_iot_mode, "NB-IoT mode", true),
	HIO_CONFIG_ITEM_BOOL("lte-m-mode", m_config_interim.lte_m_mode, "LTE-M mode", false),
	HIO_CONFIG_ITEM_BOOL("autoconn", m_config_interim.autoconn, "auto-connect feature", false),
	HIO_CONFIG_ITEM_STRING("plmnid", m_config_interim.plmnid,
			       "network PLMN ID (format: 5-6 digits)", "23003"),
	HIO_CONFIG_ITEM_STRING("apn", m_config_interim.apn, "network APN", "hardwario"),
	HIO_CONFIG_ITEM_ENUM("auth", m_config_interim.auth, m_enum_auth_items,
			     "authentication protocol", HIO_LTE_CONFIG_AUTH_NONE),
	HIO_CONFIG_ITEM_STRING("username", m_config_interim.username, "username", ""),
	HIO_CONFIG_ITEM_STRING("password", m_config_interim.password, "password", ""),
	HIO_CONFIG_ITEM_STRING("addr", m_config_interim.addr, "default IP address",
			       "192.168.192.4"),
	HIO_CONFIG_ITEM_INT("port", m_config_interim.port, 1, 65536, "default UDP port", 5002),
	HIO_CONFIG_ITEM_BOOL("modemtrace", m_config_interim.modemtrace, "modemtrace", false),
};

int hio_lte_config_cmd_show(const struct shell *shell, size_t argc, char **argv)
{
	for (int i = 0; i < ARRAY_SIZE(m_config_items); i++) {
		hio_config_show_item(shell, &m_config_items[i]);
	}

	return 0;
}

int hio_lte_config_cmd(const struct shell *shell, size_t argc, char **argv)
{
	return hio_config_cmd_config(m_config_items, ARRAY_SIZE(m_config_items), shell, argc, argv);
}

static int h_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	return hio_config_h_set(m_config_items, ARRAY_SIZE(m_config_items), key, len, read_cb,
				cb_arg);
}

static int h_commit(void)
{
	LOG_DBG("Loaded settings in full");
	memcpy(&g_hio_lte_config, &m_config_interim, sizeof(struct hio_lte_config));
	return 0;
}

static int h_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	return hio_config_h_export(m_config_items, ARRAY_SIZE(m_config_items), export_func);
}

int hio_lte_config_init(void)
{
	int ret;

	LOG_INF("System initialization");

	for (int i = 0; i < ARRAY_SIZE(m_config_items); i++) {
		hio_config_init_item(&m_config_items[i]);
	}

	static struct settings_handler sh = {
		.name = SETTINGS_PFX,
		.h_set = h_set,
		.h_commit = h_commit,
		.h_export = h_export,
	};

	ret = settings_register(&sh);
	if (ret) {
		LOG_ERR("Call `settings_register` failed: %d", ret);
		return ret;
	}

	ret = settings_load_subtree(SETTINGS_PFX);
	if (ret) {
		LOG_ERR("Call `settings_load_subtree` failed: %d", ret);
		return ret;
	}

	hio_config_append_show(SETTINGS_PFX, hio_lte_config_cmd_show);

	return 0;
}
