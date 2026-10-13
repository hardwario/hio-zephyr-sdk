/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_nvs.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>

static const char *type_str(enum hio_nvs_item_type type)
{
	switch (type) {
	case HIO_NVS_TYPE_INT32:
		return "int32";
	case HIO_NVS_TYPE_UINT32:
		return "uint32";
	case HIO_NVS_TYPE_INT64:
		return "int64";
	case HIO_NVS_TYPE_FLOAT:
		return "float";
	}

	return "?";
}

static int show_cb(const struct hio_nvs *module, const struct hio_nvs_item *item, void *user_data)
{
	const struct shell *shell = user_data;
	char value[32];

	if (hio_nvs_item_format(item, value, sizeof(value)) < 0) {
		value[0] = '\0';
	}

	shell_print(shell, "%-8s %-24s %-20s %s", module->name, item->name, value,
		    type_str(item->type));

	return 0;
}

static int noop_cb(const struct hio_nvs *module, const struct hio_nvs_item *item, void *user_data)
{
	return 0;
}

static int cmd_show(const struct shell *shell, size_t argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;

	if (hio_nvs_iter_items(filter, noop_cb, NULL) == -ENOENT) {
		shell_error(shell, "unknown module: %s", filter);
		return -ENOENT;
	}

	shell_print(shell, "%-8s %-24s %-20s %s", "module", "item", "value", "type");

	return hio_nvs_iter_items(filter, show_cb, (void *)shell);
}

static int cmd_save(const struct shell *shell, size_t argc, char **argv)
{
	int ret = hio_nvs_save();
	if (ret) {
		shell_error(shell, "save failed: %d", ret);
		return ret;
	}

	shell_print(shell, "command succeeded");

	return 0;
}

static int find_item(const struct shell *shell, const char *name,
		     const struct hio_nvs_item **item)
{
	int ret = hio_nvs_find_item(name, NULL, item);
	if (ret) {
		shell_error(shell, "unknown item: %s (see `nvs show`)", name);
	}

	return ret;
}

static int cmd_set(const struct shell *shell, size_t argc, char **argv)
{
	const struct hio_nvs_item *item;

	int ret = find_item(shell, argv[1], &item);
	if (ret) {
		return ret;
	}

	ret = hio_nvs_item_set(item, argv[2]);
	if (ret == -EINVAL) {
		shell_error(shell, "invalid %s value: %s", type_str(item->type), argv[2]);
		return ret;
	} else if (ret) {
		shell_error(shell, "save failed: %d", ret);
		return ret;
	}

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_reset(const struct shell *shell, size_t argc, char **argv)
{
	const struct hio_nvs_item *item;

	int ret = find_item(shell, argv[1], &item);
	if (ret) {
		return ret;
	}

	ret = hio_nvs_item_reset(item);
	if (ret) {
		shell_error(shell, "reset failed: %d", ret);
		return ret;
	}

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_erase(const struct shell *shell, size_t argc, char **argv)
{
	int ret = hio_nvs_erase();
	if (ret) {
		shell_error(shell, "erase failed: %d", ret);
		return ret;
	}

	shell_print(shell, "command succeeded");

	return 0;
}

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

/* clang-format off */
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_nvs,
	SHELL_CMD_ARG(show, NULL, "Show stored items: show [<module>]", cmd_show, 1, 1),
	SHELL_CMD_ARG(save, NULL, "Save all items now.", cmd_save, 1, 0),
	SHELL_CMD_ARG(set, NULL, "Set and save item: set <item> <value>", cmd_set, 3, 0),
	SHELL_CMD_ARG(reset, NULL, "Delete item and zero it: reset <item>", cmd_reset, 2, 0),
	SHELL_CMD_ARG(erase, NULL, "Erase whole NVS partition and zero all items.", cmd_erase, 1, 0),
	SHELL_SUBCMD_SET_END
);
/* clang-format on */

SHELL_CMD_REGISTER(nvs, &sub_nvs, "Persistent runtime data (NVS) commands.", print_help);
