
/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(hio_atci_cmd, CONFIG_HIO_ATCI_LOG_LEVEL);

static int compare_cmds(const char *a, const char *b)
{
	char pa = a[0];
	char pb = b[0];

	if (pa == '+' && pb != '+') {
		return -1;
	}
	if (pb == '+' && pa != '+') {
		return 1;
	}

	return strcmp(a, b);
}

static const struct hio_atci_cmd *find_nth_sorted_cmd(size_t n)
{
	size_t count, matched = 0;
	STRUCT_SECTION_COUNT(hio_atci_cmd, &count);

	const struct hio_atci_cmd *selected = NULL;

	for (size_t i = 0; i < count; i++) {
		const struct hio_atci_cmd *candidate = NULL;

		STRUCT_SECTION_FOREACH(hio_atci_cmd, item) {
			if (selected && compare_cmds(item->cmd, selected->cmd) <= 0) {
				continue;
			}
			if (!candidate || compare_cmds(item->cmd, candidate->cmd) < 0) {
				candidate = item;
			}
		}

		if (!candidate) {
			break;
		}

		if (matched == n) {
			return candidate;
		}

		selected = candidate;
		matched++;
	}

	return NULL;
}

static int help_action(const struct hio_atci *atci, bool hint)
{
	size_t count;
	STRUCT_SECTION_COUNT(hio_atci_cmd, &count);

	for (size_t i = 0; i < count; i++) {
		const struct hio_atci_cmd *item = find_nth_sorted_cmd(i);
		if (!item) {
			break;
		}

		if (hint && item->hint) {
			hio_atci_printfln(atci, "AT%s %s", item->cmd, item->hint);
		} else {
			hio_atci_printfln(atci, "AT%s", item->cmd);
		}
	}
	return 0;
}

static int at_clac_action(const struct hio_atci *atci)
{
	return help_action(atci, false);
}

HIO_ATCI_CMD_REGISTER(clac, "+CLAC", 0, at_clac_action, NULL, NULL, NULL,
		      "Command list and action");

static int at_help_action(const struct hio_atci *atci)
{
	return help_action(atci, true);
}
HIO_ATCI_CMD_REGISTER(help, "$HELP", 0, at_help_action, NULL, NULL, NULL, "This help");

#if defined(CONFIG_HIO_ATCI_CMD_SHELL)
static int at_shell_set(const struct hio_atci *atci, const char *argv)
{
	if (!argv) {
		return -EINVAL;
	}

	size_t cmd_len = strlen(argv);
	if (cmd_len > (CONFIG_SHELL_CMD_BUFF_SIZE - 1)) {
		return -ENOMEM;
	}

	if (cmd_len < 2 || argv[0] != '"' || argv[cmd_len - 1] != '"') {
		return -EINVAL;
	}

	const struct shell *sh = shell_backend_dummy_get_ptr();
	shell_backend_dummy_clear_output(sh);

	char *cmd = sh->ctx->temp_buff;

	strncpy(cmd, argv + 1, cmd_len - 2);
	cmd[cmd_len - 2] = '\0';

	LOG_INF("cmd: %s", cmd);

	shell_execute_cmd(sh, cmd);

	size_t output_len = 0;
	const char *output = shell_backend_dummy_get_output(sh, &output_len);
	if (!output) {
		LOG_ERR("Failed to get shell output");
		return -ENOMEM;
	}

	const char *start = output;
	const char *end = output + output_len;

	while (start < end) {
		const char *newline = strstr(start, "\r\n");

		if (!newline) {
			hio_atci_printfln(atci, "$shell: \"%.*s\"", (int)(end - start), start);
			break;
		}

		size_t line_len = newline - start;

		if (line_len > 0) {
			hio_atci_printfln(atci, "$shell: \"%.*s\"", (int)line_len, start);
		}

		start = newline + 2; // skip \r\n
	}
	return 0;
}
HIO_ATCI_CMD_REGISTER(shell, "$SHELL", CONFIG_HIO_ATCI_CMD_SHELL_ACL, NULL, at_shell_set, NULL,
		      NULL, "Shell command");
#endif /* CONFIG_HIO_ATCI_CMD_SHELL */
