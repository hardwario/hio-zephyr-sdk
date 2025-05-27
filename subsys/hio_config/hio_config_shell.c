/* HIO includes */
#include <hio/hio_config.h>
#include <hio/hio_util.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/slist.h>

/* Standard includes */
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_DECLARE(hio_config);

static int item_print_value(const struct shell *shell, const struct hio_config *module,
			    const struct hio_config_item *item)
{

	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		shell_print(shell, "%s config %s %d", module->name, item->name,
			    *(int *)item->variable);
		break;

	case HIO_CONFIG_TYPE_FLOAT:
		shell_print(shell, "%s config %s %.2f", module->name, item->name,
			    (double)*(float *)item->variable);
		break;

	case HIO_CONFIG_TYPE_BOOL:
		shell_print(shell, "%s config %s %s", module->name, item->name,
			    *(bool *)item->variable ? "true" : "false");
		break;

	case HIO_CONFIG_TYPE_ENUM: {
		int32_t val = 0;
		memcpy(&val, item->variable, item->size);

		shell_print(shell, "%s config %s \"%s\"", module->name, item->name,
			    item->enums[val]);
		break;
	}
	case HIO_CONFIG_TYPE_STRING:
		shell_print(shell, "%s config %s \"%s\"", module->name, item->name,
			    (char *)item->variable);
		break;

	case HIO_CONFIG_TYPE_HEX:
		shell_fprintf(shell, SHELL_NORMAL, "%s config %s ", module->name, item->name);
		for (int i = 0; i < item->size; i++) {
			shell_fprintf(shell, SHELL_NORMAL, "%02x", ((uint8_t *)item->variable)[i]);
		}
		shell_fprintf(shell, SHELL_NORMAL, "\n");
		break;
	}

	return 0;
}

static int item_print_help(const struct shell *shell, const struct hio_config_item *item)
{
	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		shell_print(shell, "  %-18s:%s <%d~%d>", item->name, item->help, item->min,
			    item->max);
		break;

	case HIO_CONFIG_TYPE_FLOAT:
		shell_print(shell, "  %-18s:%s <%.2f~%.2f>", item->name, item->help,
			    (double)item->min, (double)item->max);
		break;

	case HIO_CONFIG_TYPE_BOOL:
		shell_print(shell, "  %-18s:%s <true/false>", item->name, item->help);
		break;

	case HIO_CONFIG_TYPE_ENUM:
		shell_print(shell, "  %-18s:%s", item->name, item->help);
		for (int i = 0; i < item->max; i++) {
			if (strlen(item->enums[i])) {
				shell_print(shell, "                     - %s", item->enums[i]);
			}
		}
		break;

	case HIO_CONFIG_TYPE_STRING:
		shell_print(shell, "  %-18s:%s", item->name, item->help);
		break;

	case HIO_CONFIG_TYPE_HEX:
		shell_print(shell, "  %-18s:%s (len: %d B)", item->name, item->help, item->size);
		break;
	}

	return 0;
}

int hio_config_shell_cmd(const struct shell *shell, size_t argc, char **argv)
{
	LOG_INF("hio_config_shell_cmd: argc: %d", argc);

	for (int i = 0; i < argc; i++) {
		LOG_INF("argv[%d]: %s", i, argv[i]);
	}

	LOG_INF("cmd_buff: %s", shell->ctx->cmd_buff);

	const char *module_name = shell->ctx->cmd_buff;

	int ret;
	struct hio_config *module;

	ret = hio_config_find_module(module_name, &module);
	if (ret) {
		shell_error(shell, "module not found: %s", module_name);
		return ret;
	}

	/* Print parameter(s) */
	if (argc == 2) {
		bool has_wildcard = argv[1][strlen(argv[1]) - 1] == '*';

		if (strcmp(argv[1], "show") == 0) {
			for (int i = 0; i < module->nitems; i++) {
				item_print_value(shell, module, &module->items[i]);
			}
			return 0;
		}

		struct hio_config_item *item;
		for (int i = 0; i < module->nitems; i++) {
			item = &module->items[i];

			if ((!has_wildcard && strcmp(argv[1], item->name) == 0) ||
			    (has_wildcard &&
			     strncmp(argv[1], item->name, strlen(argv[1]) - 1) == 0)) {
				item_print_value(shell, module, item);
				return 0;
			}
		}
	}

	/* Write parameter(s) */
	if (argc == 3) {
		bool has_wildcard = argv[1][strlen(argv[1]) - 1] == '*';

		struct hio_config_item *item;
		for (int i = 0; i < module->nitems; i++) {
			item = &module->items[i];

			if ((!has_wildcard && strcmp(argv[1], item->name) == 0) ||
			    (has_wildcard &&
			     strncmp(argv[1], item->name, strlen(argv[1]) - 1) == 0)) {
				const char *err_msg = NULL;
				ret = hio_config_item_parse(item, argv[2], &err_msg);
				if (ret) {
					if (err_msg) {
						shell_error(shell, "%s", err_msg);
					} else {
						shell_error(shell, "Invalid value");
					}
					item_print_help(shell, item);
					return ret;
				}
				return 0;
			}
		}
	}

	/* No parameter name, print help */
	for (int i = 0; i < module->nitems; i++) {
		item_print_help(shell, &module->items[i]);
	}
	return 0;
}

static int cmd_modules_cb(const struct hio_config *module, void *user_data)
{
	shell_print((const struct shell *)user_data, "%s", module->name);
	return 0;
}

static int cmd_modules(const struct shell *shell, size_t argc, char **argv)
{
	return hio_config_iter_modules(cmd_modules_cb, (void *)shell);
}

static int print_item_cb(const struct hio_config *module, const struct hio_config_item *item,
			 void *user_data)
{
	return item_print_value((const struct shell *)user_data, module, item);
}

static int cmd_show(const struct shell *shell, size_t argc, char **argv)
{
	return hio_config_iter_items(NULL, print_item_cb, (void *)shell);
}

static int cmd_save(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = hio_config_save();
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	return 0;
}

static int cmd_reset(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = hio_config_reset();
	if (ret) {
		LOG_ERR("Call `hio_config_reset` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

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
	sub_config,

	SHELL_CMD_ARG(modules, NULL,
	              "Show all modules.",
	              cmd_modules, 1, 0),

	SHELL_CMD_ARG(show, NULL,
	              "Show all configuration.",
	              cmd_show, 1, 0),

	SHELL_CMD_ARG(save, NULL,
	              "Save all configuration.",
	              cmd_save, 1, 0),

	SHELL_CMD_ARG(reset, NULL,
	              "Reset all configuration.",
	              cmd_reset, 1, 0),

	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(config, &sub_config, "Configuration commands.", print_help);
