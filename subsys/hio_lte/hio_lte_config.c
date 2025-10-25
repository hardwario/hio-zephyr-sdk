/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_lte_config.h"
#include "hio_lte_tok.h"

/* HIO includes */
#include <hio/hio_config.h>
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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

/* clang-format off */
static const char *m_enum_auth_items[] = {
	[HIO_LTE_CONFIG_AUTH_NONE] = "none",
	[HIO_LTE_CONFIG_AUTH_PAP] = "pap",
	[HIO_LTE_CONFIG_AUTH_CHAP] = "chap"
};

static const char *m_enum_attach_policy_items[] = {
	[HIO_LTE_ATTACH_POLICY_AGGRESSIVE] = "aggressive",
	[HIO_LTE_ATTACH_POLICY_PERIODIC_2H] = "periodic-2h",
	[HIO_LTE_ATTACH_POLICY_PERIODIC_6H] = "periodic-6h",
	[HIO_LTE_ATTACH_POLICY_PERIODIC_12H] = "periodic-12h",
	[HIO_LTE_ATTACH_POLICY_PERIODIC_1D] = "periodic-1d",
	[HIO_LTE_ATTACH_POLICY_PROGRESSIVE] = "progressive",
};
/* clang-format on */

static int mode_parse_cb(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	size_t len = strlen(argv);
	if (len >= item->size) {
		*err_msg = "Value too long";
		return -ENOMEM;
	}

	const char *p = argv;
	bool ok = false;
	while (p) {

		if (!strncmp(p, "lte-m", 5)) {
			ok = true;
			p += 5;
		} else if (!strncmp(p, "nb-iot", 6)) {
			ok = true;
			p += 6;
		} else {
			*err_msg = "Invalid mode";
			return -EINVAL;
		}

		if (hio_lte_tok_end(p)) {
			break;
		}

		if (!(p = hio_lte_tok_sep(p))) {
			*err_msg = "Expected comma";
			return -EINVAL;
		}
	}

	if (!ok) {
		*err_msg = "Need at least one of modes lte-m or nb-iot";
		return -EINVAL;
	}

	strncpy(item->variable, argv, item->size - 1);
	((char *)item->variable)[item->size - 1] = '\0';

	return 0;
}

static bool is_supported_band(uint8_t band)
{
	static const uint8_t support_bands[] = {1,  2,  3,  4,  5,  8,  12, 13,
						17, 18, 19, 20, 25, 26, 28, 66};
	for (size_t i = 0; i < ARRAY_SIZE(support_bands); i++) {
		if (band == support_bands[i]) {
			return true;
		}
	}
	return false;
}

int bands_parse_cb(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	size_t len = strlen(argv);
	if (len >= item->size) {
		*err_msg = "Value too long";
		return -ENOMEM;
	}

	if (!len) {
		memset(item->variable, 0, item->size);
		return 0;
	}

	const char *p = argv;

	bool def;
	long band;

	while (p) {
		if (!(p = hio_lte_tok_num(p, &def, &band)) || !def || band < 0 || band > 255) {
			*err_msg = "Invalid number format";
			return -EINVAL;
		}

		if (!is_supported_band((uint8_t)band)) {
			*err_msg = "Band is not supported";
			return -EINVAL;
		}

		if (hio_lte_tok_end(p)) {
			break;
		}

		if (!(p = hio_lte_tok_sep(p))) {
			*err_msg = "Expected comma or end of string";
			return -EINVAL;
		}
	}

	strncpy(item->variable, argv, item->size - 1);
	((char *)item->variable)[item->size - 1] = '\0';

	return 0;
}

static int network_parse_cb(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	size_t len = strlen(argv);

	if (len >= item->size) {
		return -ENOMEM;
	}

	if (!len) {
		memset(item->variable, 0, item->size);
		return 0;
	}
	if (len < 5 || len > 6) {
		*err_msg = "PLMN ID must be 5-6 digits";
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		if (!isdigit(argv[i])) {
			*err_msg = "PLMN ID must be digits";
			return -EINVAL;
		}
	}

	strncpy(item->variable, argv, item->size - 1);
	((char *)item->variable)[item->size - 1] = '\0';

	return 0;
}

static struct hio_config_item m_config_items[] = {
	HIO_CONFIG_ITEM_BOOL("test", m_config_interim.test, "LTE test", false),
	HIO_CONFIG_ITEM_STRING_PARSE_CB("mode", m_config_interim.mode,
					"supported modes, ordered by priority\n"
					"                     - lte-m,nb-iot\n"
					"                     - nb-iot,lte-m\n"
					"                     - lte-m\n"
					"                     - nb-iot",
					CONFIG_HIO_LTE_DEFAULT_MODE, mode_parse_cb),
	HIO_CONFIG_ITEM_STRING_PARSE_CB(
		"bands", m_config_interim.bands,
		"supported bands (\"\" means no bands lock or listed with comma separator): \n"
		"                     - LTE-M:  "
		"1,2,3,4,5,8,12,13,18,19,20,25,26,28,66\n"
		"                     - NB-IoT: "
		"1,2,3,4,5,8,12,13,17,19,20,25,26,28,66",
		CONFIG_HIO_LTE_DEFAULT_BANDS, bands_parse_cb),

	HIO_CONFIG_ITEM_STRING_PARSE_CB(
		"network", m_config_interim.network,
		"network (\"\" means automatic network selection or PLMN ID (format: 5-6 digits)",
		CONFIG_HIO_LTE_DEFAULT_NETWORK, network_parse_cb),
	HIO_CONFIG_ITEM_STRING("apn", m_config_interim.apn, "network APN",
			       CONFIG_HIO_LTE_DEFAULT_APN),
	HIO_CONFIG_ITEM_ENUM("auth", m_config_interim.auth, m_enum_auth_items,
			     "authentication protocol", HIO_LTE_CONFIG_AUTH_NONE),
	HIO_CONFIG_ITEM_STRING("username", m_config_interim.username, "username", ""),
	HIO_CONFIG_ITEM_STRING("password", m_config_interim.password, "password", ""),
	HIO_CONFIG_ITEM_STRING("addr", m_config_interim.addr, "default IP address",
			       CONFIG_HIO_LTE_DEFAULT_ADDR),
	HIO_CONFIG_ITEM_ENUM("attach-policy", m_config_interim.attach_policy,
			     m_enum_attach_policy_items, "attach policy",
			     HIO_LTE_ATTACH_POLICY_PERIODIC_2H),
	// HIO_CONFIG_ITEM_INT("port", m_config_interim.port, 1, 65536, "default UDP port", 5002),
	HIO_CONFIG_ITEM_BOOL("modemtrace", m_config_interim.modemtrace, "enable modem trace",
			     false),
};

int hio_lte_config_init(void)
{
	LOG_INF("System initialization");

	static struct hio_config config = {
		.name = SETTINGS_PFX,
		.items = m_config_items,
		.nitems = ARRAY_SIZE(m_config_items),

		.interim = &m_config_interim,
		.final = &g_hio_lte_config,
		.size = sizeof(g_hio_lte_config),
	};

	hio_config_register(&config);

	return 0;
}
