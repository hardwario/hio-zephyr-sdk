/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_config.h"

/* HIO includes */
#include <hio/hio_config.h>

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

LOG_MODULE_REGISTER(hio_cloud_config, CONFIG_HIO_CLOUD_LOG_LEVEL);

#define SETTINGS_PFX "cloud"

struct hio_cloud_config g_hio_cloud_config;
static struct hio_cloud_config m_config_interim;

/* clang-format off */
static const char *m_enum_mode_items[] = {
	[HIO_CLOUD_PROTOCOL_FLAP_HASH] = "flap-hash",
	[HIO_CLOUD_PROTOCOL_FLAP_DTLS] = "flap-dtls",
};
/* clang-format on */

static struct hio_config_item m_config_items[] = {
	HIO_CONFIG_ITEM_ENUM("protocol", m_config_interim.protocol, m_enum_mode_items,
			     "transfer protocol", HIO_CLOUD_PROTOCOL_FLAP_HASH),
	HIO_CONFIG_ITEM_STRING("addr", m_config_interim.addr, "default IP address",
			       CONFIG_HIO_CLOUD_DEFAULT_ADDR),
	HIO_CONFIG_ITEM_INT("port-flap-hash", m_config_interim.port_signed, 1, 65536,
			    "default UDP port for flap-hash mode",
			    CONFIG_HIO_CLOUD_DEFAULT_PORT_HASH),
	HIO_CONFIG_ITEM_INT("port-flap-dtls", m_config_interim.port_dtls, 1, 65536,
			    "default UDP port for flap-dtls mode",
			    CONFIG_HIO_CLOUD_DEFAULT_PORT_DTLS),
};

int hio_cloud_config_init(void)
{
	LOG_INF("System initialization");

	static struct hio_config config = {
		.name = SETTINGS_PFX,
		.items = m_config_items,
		.nitems = ARRAY_SIZE(m_config_items),

		.interim = &m_config_interim,
		.final = &g_hio_cloud_config,
		.size = sizeof(g_hio_cloud_config),
	};

	hio_config_register(&config);

	return 0;
}
