/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_atci_config.h"

/* HIO includes */
#include <hio/hio_atci.h>
#include <hio/hio_config.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_atci_config, CONFIG_HIO_ATCI_LOG_LEVEL);

#define SETTINGS_PFX "atci"

struct hio_atci_config g_hio_atci_config;
static struct hio_atci_config m_config_interim;

/* clang-format off */

static struct hio_config_item m_config_items[] = {

#if defined(CONFIG_HIO_ATCI_LOG_BACKEND)
	HIO_CONFIG_ITEM_BOOL("log", m_config_interim.log, "log output on ATCI",
			     IS_ENABLED(CONFIG_HIO_ATCI_LOG_DEFAULT)),
#endif

#if defined(CONFIG_HIO_ATCI_LOGIN)
	HIO_CONFIG_ITEM_STRING("passphrase-hash", m_config_interim.passphrase_hash,
			       "authentication passphrase (SHA-256 hash hex string)",
			       CONFIG_HIO_ATCI_LOGIN_DEFAULT_PASSPHRASE_HASH),
#endif

};

/* clang-format on */

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

	static struct hio_config config = {
		.name = SETTINGS_PFX,
		.items = m_config_items,
		.nitems = ARRAY_SIZE(m_config_items),

		.interim = &m_config_interim,
		.final = &g_hio_atci_config,
		.size = sizeof(g_hio_atci_config),
	};

	ret = hio_config_register(&config);
	if (ret) {
		LOG_ERR("Call `hio_config_register` failed: %d", ret);
	}

#if defined(CONFIG_HIO_ATCI_LOG_BACKEND)
	/* The log backend starts muted and is released here once the stored
	 * setting is known, so no boot log leaks out when "log" is false. */
	hio_atci_log_enable(NULL,
			    ret ? IS_ENABLED(CONFIG_HIO_ATCI_LOG_DEFAULT) : g_hio_atci_config.log);
#endif

	return 0;
}

#if defined(CONFIG_SHELL)

static int print_help(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	shell_help(shell);

	return 0;
}

#if defined(CONFIG_HIO_ATCI_LOG_BACKEND)

static int cmd_log(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		STRUCT_SECTION_FOREACH(hio_atci, atci) {
			if (atci->log_backend) {
				shell_print(shell, "%s: %s", atci->name,
					    hio_atci_log_is_enabled(atci) ? "on" : "off");
			}
		}
		return 0;
	}

	bool enable;

	if (strcmp(argv[1], "on") == 0) {
		enable = true;
	} else if (strcmp(argv[1], "off") == 0) {
		enable = false;
	} else {
		shell_error(shell, "invalid argument: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	hio_atci_log_enable(NULL, enable);

	shell_info(shell, "command succeeded");

	return 0;
}

#endif /* CONFIG_HIO_ATCI_LOG_BACKEND */

/* clang-format off */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_atci,

#if defined(CONFIG_HIO_CONFIG_SHELL)
	HIO_CONFIG_SHELL_CMD_ARG,
#endif

#if defined(CONFIG_HIO_ATCI_LOG_BACKEND)
	SHELL_CMD_ARG(log, NULL, "Log output on ATCI (runtime): [on|off].", cmd_log, 1, 1),
#endif

	SHELL_SUBCMD_SET_END);

/* clang-format on */

SHELL_CMD_REGISTER(atci, &sub_atci, "ATCI commands.", print_help);

#endif /* CONFIG_SHELL */

/* Load after hio config */
BUILD_ASSERT(CONFIG_HIO_CONFIG_INIT_PRIORITY < CONFIG_HIO_ATCI_CONFIG_INIT_PRIORITY);

SYS_INIT(init, APPLICATION, CONFIG_HIO_ATCI_CONFIG_INIT_PRIORITY);
