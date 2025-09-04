/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HARDWARIO includes */
#include <hio/hio_config.h>
#include <hio/hio_atci.h>
#include <hio/hio_tok.h>
#include <hio/hio_sys.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_DECLARE(hio_config);

static int item_print_value(const struct hio_atci *atci, const struct hio_config *module,
			    const struct hio_config_item *item)
{

	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		hio_atci_printfln(atci, "$CONFIG: \"%s\",\"%s\",%d", module->name, item->name,
				  *(int *)item->variable);
		break;

	case HIO_CONFIG_TYPE_FLOAT:
		hio_atci_printfln(atci, "$CONFIG: \"%s\",\"%s\",%.2f", module->name, item->name,
				  (double)*(float *)item->variable);
		break;

	case HIO_CONFIG_TYPE_BOOL:
		hio_atci_printfln(atci, "$CONFIG: \"%s\",\"%s\",%s", module->name, item->name,
				  *(bool *)item->variable ? "true" : "false");
		break;

	case HIO_CONFIG_TYPE_ENUM: {
		int32_t val = 0;
		memcpy(&val, item->variable, item->size);
		hio_atci_printfln(atci, "$CONFIG: \"%s\",\"%s\",\"%s\"", module->name, item->name,
				  item->enums[val]);
		break;
	}
	case HIO_CONFIG_TYPE_STRING:
		hio_atci_printfln(atci, "$CONFIG: \"%s\",\"%s\",\"%s\"", module->name, item->name,
				  (char *)item->variable);
		break;

	case HIO_CONFIG_TYPE_HEX:
		hio_atci_printf(atci, "$CONFIG: \"%s\",\"%s\",\"", module->name, item->name);
		for (int i = 0; i < item->size; i++) {
			hio_atci_printf(atci, "%02x", ((uint8_t *)item->variable)[i]);
		}
		hio_atci_printfln(atci, "\"");
		break;
	}

	return 0;
}

static int at_config_set(const struct hio_atci *atci, char *argv)
{
	int ret;
	char *tmp;
	size_t tmp_len;
	hio_atci_get_tmp_buff(atci, &tmp, &tmp_len);
	const char *p = argv;
	bool def = false;

	if (!(p = hio_tok_str(p, &def, tmp, tmp_len)) || !def) {
		return -EINVAL;
	}

	struct hio_config *module = NULL;
	ret = hio_config_find_module(tmp, &module);
	if (ret) {
		hio_atci_error(atci, "\"Module not found\"");
		return ret;
	}

	if (hio_tok_end(p)) {
		for (int i = 0; i < module->nitems; i++) {
			item_print_value(atci, module, &module->items[i]);
		}
		return 0;
	}

	if (!(p = hio_tok_sep(p))) {
		return -EINVAL;
	}

	if (!(p = hio_tok_str(p, &def, tmp, tmp_len)) || !def) {
		return -EINVAL;
	}

	const struct hio_config_item *item = NULL;

	ret = hio_config_module_find_item(module, tmp, &item);
	if (ret) {
		hio_atci_error(atci, "\"Item not found\"");
		return ret;
	}

	if (hio_tok_end(p)) {
		item_print_value(atci, module, item);
		return 0;
	}

	if (!(p = hio_tok_sep(p))) {
		return -EINVAL;
	}

	if (hio_tok_is_quoted(p)) {
		/* String value */
		if (!(p = hio_tok_str(p, &def, tmp, tmp_len)) || !def) {
			return -EINVAL;
		}

		if (!hio_tok_end(p)) {
			hio_atci_error(atci, "\"Invalid value format\"");
			return -EINVAL;
		}
	} else {
		/* Non-string value */
		if (strchr(p, ',') != NULL) { /* Check for comma in value */
			hio_atci_error(atci, "\"Invalid value format\"");
			return -EINVAL;
		}
		strncpy(tmp, p, tmp_len - 1);
	}

	const char *err_msg = NULL;
	ret = hio_config_item_parse(item, tmp, &err_msg);
	if (ret) {
		if (err_msg) {
			hio_atci_errorf(atci, "\"%s\"", err_msg);
		} else {
			hio_atci_error(atci, "\"Invalid value\"");
		}
		return ret;
	}

	return 0;
}

static int print_item_cb(const struct hio_config *module, const struct hio_config_item *item,
			 void *user_data)
{
	return item_print_value((const struct hio_atci *)user_data, module, item);
}

static int at_config_read(const struct hio_atci *atci)
{
	return hio_config_iter_items(NULL, print_item_cb, (void *)atci);
}

static int at_reset_action(const struct hio_atci *atci)
{
	int ret;

	ret = hio_config_reset_without_reboot();
	if (ret) {
		LOG_ERR("Call `hio_config_reset` failed: %d", ret);
		return ret;
	}

	hio_atci_println(atci, "OK");

	k_sleep(K_SECONDS(1));

	hio_sys_reboot("Config reset");

	return ret;
}

static int at_write_action(const struct hio_atci *atci)
{
	int ret;

	ret = hio_config_save_without_reboot();
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		return ret;
	}

	hio_atci_println(atci, "OK");

	k_sleep(K_SECONDS(1));

	hio_sys_reboot("Config save");

	return ret;
}

HIO_ATCI_CMD_REGISTER(config_f, "&F", 0, at_reset_action, NULL, NULL, NULL,
		      "Reset all configuration.");
HIO_ATCI_CMD_REGISTER(config_w, "&W", 0, at_write_action, NULL, NULL, NULL,
		      "Save all configuration.");
HIO_ATCI_CMD_REGISTER(config, "$CONFIG", 0, NULL, at_config_set, at_config_read, NULL,
		      "Configuration parameters.");
