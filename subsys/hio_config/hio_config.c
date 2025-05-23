/* HIO includes */
#include <hio/hio_config.h>
#include <hio/hio_util.h>

/* Zephyr includes */
#include <zephyr/fs/fs.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(hio_config, CONFIG_HIO_CONFIG_LOG_LEVEL);

struct show_item {
	const char *name;
	hio_config_show_cb cb;
	sys_snode_t node;
};

static sys_slist_t m_show_list = SYS_SLIST_STATIC_INIT(&m_show_list);

static int save(bool reboot)
{
	int ret;

	ret = settings_save();
	if (ret) {
		LOG_ERR("Call `settings_save` failed: %d", ret);
		return ret;
	}

	if (reboot) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

static int reset(bool reboot)
{
	int ret;

#if defined(CONFIG_SETTINGS_FILE)
	/* Settings in external FLASH as a LittleFS file */
	ret = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
	if (ret) {
		LOG_WRN("Call `fs_unlink` failed: %d", ret);
	}

	/* Needs to be static so it is zero-ed */
	static struct fs_file_t file;
	ret = fs_open(&file, CONFIG_SETTINGS_FILE_PATH, FS_O_CREATE);
	if (ret) {
		LOG_ERR("Call `fs_open` failed: %d", ret);
		return ret;
	}

	ret = fs_close(&file);
	if (ret) {
		LOG_ERR("Call `fs_close` failed: %d", ret);
		return ret;
	}
#else
	/* Settings in the internal FLASH partition */
	const struct flash_area *fa;
	ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	if (ret) {
		LOG_ERR("Call `flash_area_open` failed: %d", ret);
		return ret;
	}

	ret = flash_area_erase(fa, 0, FIXED_PARTITION_SIZE(storage_partition));
	if (ret < 0) {
		LOG_ERR("Call `flash_area_erase` failed: %d", ret);
		return ret;
	}

	flash_area_close(fa);
#endif /* defined(CONFIG_SETTINGS_FILE) */

	if (reboot) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

int hio_config_save(bool reboot)
{
	int ret;

	ret = save(reboot);
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_config_reset(bool reboot)
{
	int ret;

	ret = reset(reboot);
	if (ret) {
		LOG_ERR("Call `reset` failed: %d", ret);
		return ret;
	}

	return 0;
}

void hio_config_append_show(const char *name, hio_config_show_cb cb)
{
	struct show_item *item = k_malloc(sizeof(*item));
	if (item == NULL) {
		LOG_ERR("Call `k_malloc` failed");
		return;
	}

	item->name = name;
	item->cb = cb;

	sys_slist_append(&m_show_list, &item->node);
}

int hio_config_show_item(const struct shell *shell, const struct hio_config_item *item)
{
	char mod[32];
	{ /* truncate anything after - from the module name */
		mod[31] = 0;
		strncpy(mod, item->module, sizeof(mod) - 1);
		char *s;
		if ((s = strstr(mod, "-")) != NULL) {
			*s = 0;
		}
	}

	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		shell_print(shell, "%s config %s %d", mod, item->name, *(int *)item->variable);
		break;

	case HIO_CONFIG_TYPE_FLOAT:
		shell_print(shell, "%s config %s %.2f", mod, item->name, *(double *)item->variable);
		break;

	case HIO_CONFIG_TYPE_BOOL:
		shell_print(shell, "%s config %s %s", mod, item->name,
			    *(bool *)item->variable ? "true" : "false");
		break;

	case HIO_CONFIG_TYPE_ENUM: {
		int32_t val = 0;
		memcpy(&val, item->variable, item->size);

		shell_print(shell, "%s config %s \"%s\"", mod, item->name, item->enums[val]);
		break;
	}
	case HIO_CONFIG_TYPE_STRING:
		shell_print(shell, "%s config %s \"%s\"", mod, item->name, (char *)item->variable);
		break;

	case HIO_CONFIG_TYPE_HEX:
		shell_fprintf(shell, SHELL_NORMAL, "%s config %s ", mod, item->name);
		for (int i = 0; i < item->size; i++) {
			shell_fprintf(shell, SHELL_NORMAL, "%02x", ((uint8_t *)item->variable)[i]);
		}
		shell_fprintf(shell, SHELL_NORMAL, "\n");
		break;
	}

	return 0;
}

int hio_config_help_item(const struct shell *shell, const struct hio_config_item *item)
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

static int parse_int(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	for (size_t i = 0; i < strlen(argv); i++) {
		if (!isdigit((int)argv[i])) {
			shell_error(shell, "Invalid format");
			hio_config_help_item(shell, item);
			return -EINVAL;
		}
	}

	long value = strtol(argv, NULL, 10);
	if (value < item->min || value > item->max) {
		shell_error(shell, "Invalid range");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	*((int *)item->variable) = (int)value;

	return 0;
}

static int parse_float(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	float value;
	int ret = sscanf(argv, "%f", &value);

	if (ret != 1) {
		shell_error(shell, "Invalid value");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	if (value < item->min || value > item->max) {
		shell_error(shell, "Invalid range");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	*((float *)item->variable) = value;

	return 0;
}

static int parse_bool(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	bool is_false = !strcmp(argv, "false");
	bool is_true = !strcmp(argv, "true");

	if (is_false) {
		*((bool *)item->variable) = false;
	} else if (is_true) {
		*((bool *)item->variable) = true;
	} else {
		shell_error(shell, "invalid format");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	return 0;
}

static int parse_enum(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	int value = -1;

	for (size_t i = 0; i < item->max; i++) {
		if (strcmp(argv, item->enums[i]) == 0) {
			value = i;
			break;
		}
	}

	if (value < 0) {
		shell_error(shell, "invalid option");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	memcpy(item->variable, &value, item->size);

	return 0;
}

static int parse_string(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	if (strlen(argv) + 1 > item->size) {
		shell_error(shell, "value too long");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	strcpy(item->variable, argv);

	return 0;
}

static int parse_hex(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	int ret = hio_hex2buf(argv, item->variable, item->size, true);

	if (ret != item->size) {
		shell_error(shell, "Length does not match.");
		hio_config_help_item(shell, item);
		return -EINVAL;
	}

	return 0;
}

int hio_config_parse_item(const struct shell *shell, char *argv, const struct hio_config_item *item)
{
	if (item->parse_cb != NULL) {
		return item->parse_cb(shell, argv, item);
	}

	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		return parse_int(shell, argv, item);
	case HIO_CONFIG_TYPE_FLOAT:
		return parse_float(shell, argv, item);
	case HIO_CONFIG_TYPE_BOOL:
		return parse_bool(shell, argv, item);
	case HIO_CONFIG_TYPE_ENUM:
		return parse_enum(shell, argv, item);
	case HIO_CONFIG_TYPE_STRING:
		return parse_string(shell, argv, item);
	case HIO_CONFIG_TYPE_HEX:
		return parse_hex(shell, argv, item);
	}

	return -EINVAL;
}

int hio_config_init_item(const struct hio_config_item *item)
{
	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		*(int *)item->variable = item->default_int;
		break;
	case HIO_CONFIG_TYPE_FLOAT:
		*(float *)item->variable = item->default_float;
		break;
	case HIO_CONFIG_TYPE_BOOL:
		*(bool *)item->variable = item->default_bool;
		break;
	case HIO_CONFIG_TYPE_ENUM:
		memcpy(item->variable, &item->default_enum, item->size);
		break;
	case HIO_CONFIG_TYPE_STRING:
		strcpy(item->variable, item->default_string);
		break;
	case HIO_CONFIG_TYPE_HEX:
		memcpy(item->variable, item->default_hex, item->size);
		break;
	}

	return 0;
}

int hio_config_cmd_config(const struct hio_config_item *items, int nitems,
			  const struct shell *shell, size_t argc, char **argv)
{
	/* No parameter name, print help */
	if (argc == 1) {
		for (int i = 0; i < nitems; i++) {
			hio_config_help_item(shell, &items[i]);
		}

		return 0;
	}

	/* Print parameter(s) */
	else if (argc == 2) {
		bool found_any = false;
		bool has_wildcard = argv[1][strlen(argv[1]) - 1] == '*';

		if (strcmp(argv[1], "show") == 0) {
			for (int i = 0; i < nitems; i++) {
				hio_config_show_item(shell, &items[i]);
			}

			return 0;
		}

		for (int i = 0; i < nitems; i++) {
			if ((!has_wildcard && strcmp(argv[1], items[i].name) == 0) ||
			    (has_wildcard &&
			     strncmp(argv[1], items[i].name, strlen(argv[1]) - 1) == 0)) {
				hio_config_show_item(shell, &items[i]);
				found_any = true;
			}
		}

		/* Print help item list if not found any item */
		if (!found_any) {
			for (int i = 0; i < nitems; i++) {
				hio_config_help_item(shell, &items[i]);
			}

			return 0;
		}
	}

	/* Write parameter(s) */
	else if (argc == 3) {
		bool found_any = false;
		bool has_wildcard = argv[1][strlen(argv[1]) - 1] == '*';

		for (int i = 0; i < nitems; i++) {

			if ((!has_wildcard && strcmp(argv[1], items[i].name) == 0) ||
			    (has_wildcard &&
			     strncmp(argv[1], items[i].name, strlen(argv[1]) - 1) == 0)) {

				found_any = true;

				hio_config_parse_item(shell, argv[2], &items[i]);
			}
		}

		/* Print help item list if not found any item */
		if (!found_any) {
			for (int i = 0; i < nitems; i++) {
				hio_config_help_item(shell, &items[i]);
			}

			return 0;
		}
	}

	return 0;
}

int hio_config_h_set(const struct hio_config_item *items, int nitems, const char *key, size_t len,
		     settings_read_cb read_cb, void *cb_arg)
{
	int ret;
	const char *next;

	for (int i = 0; i < nitems; i++) {
		if (settings_name_steq(key, items[i].name, &next) && !next) {
			if (len != items[i].size) {
				return -EINVAL;
			}
			ret = read_cb(cb_arg, items[i].variable, len);
			if (ret < 0) {
				LOG_ERR("Call `read_cb` failed: %d", ret);
				return ret;
			}
			return 0;
		}
	}

	return -ENOENT;
}

int hio_config_h_export(const struct hio_config_item *items, int nitems,
			int (*export_func)(const char *name, const void *val, size_t val_len))
{
	int ret;

	static char name_concat[64];

	for (int i = 0; i < nitems; i++) {
		snprintf(name_concat, sizeof(name_concat), "%s/%s", items[i].module, items[i].name);
		ret = export_func(name_concat, items[i].variable, items[i].size);
		if (ret) {
			LOG_ERR("Call `export_func` failed: %d", ret);
			return ret;
		}
	}

	return 0;
}

static int cmd_modules(const struct shell *shell, size_t argc, char **argv)
{
	struct show_item *item;
	SYS_SLIST_FOR_EACH_CONTAINER(&m_show_list, item, node) {
		shell_print(shell, "%s", item->name);
	}

	return 0;
}

static int cmd_show(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	struct show_item *item;
	SYS_SLIST_FOR_EACH_CONTAINER(&m_show_list, item, node) {
		if (item->cb != NULL) {
			ret = item->cb(shell, argc, argv);
			if (ret) {
				LOG_ERR("Call `item->cb` failed: %d", ret);
				shell_error(shell, "command failed");
				return ret;
			}
		}
	}

	return 0;
}

static int cmd_save(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = save(true);
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

	ret = reset(true);
	if (ret) {
		LOG_ERR("Call `reset` failed: %d", ret);
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

/* clang-format on */

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Call `settings_subsys_init` failed: %d", ret);
		return ret;
	}

	return 0;
}

SYS_INIT(init, APPLICATION, CONFIG_HIO_CONFIG_INIT_PRIORITY);
