/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* Required for strtok_r */

/* HIO includes */
#include <hio/hio_config.h>
#include <hio/hio_util.h>
#include <hio/hio_sys.h>

/* Zephyr includes */
#include <zephyr/fs/fs.h>
/* NVS moved from zephyr/fs to zephyr/kvss in Zephyr 4.3 (NCS 3.4) */
#if __has_include(<zephyr/kvss/nvs.h>)
#include <zephyr/kvss/nvs.h>
#else
#include <zephyr/fs/nvs.h>
#endif
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/crc.h>

/* Standard includes */
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_config, CONFIG_HIO_CONFIG_LOG_LEVEL);

#define CONFIG_EVENT_READY BIT(0)
static K_EVENT_DEFINE(m_event);
static sys_slist_t m_list = SYS_SLIST_STATIC_INIT(&m_list);
static hio_config_access_cb m_access_cb;
/* Incremented by every value write and storage reset; see hio_config_get_gen() */
static uint32_t m_gen;
/* m_gen at the last successful save; differs from m_gen while dirty */
static uint32_t m_saved_gen;

int hio_config_item_access(const struct hio_config *module, const struct hio_config_item *item)
{
	return m_access_cb ? m_access_cb(module, item) : HIO_CONFIG_ACCESS_RW;
}

void hio_config_set_access_cb(hio_config_access_cb cb)
{
	m_access_cb = cb;
}

/* Write the default value of the item to dst (laid out like item->variable) */
static int item_default(const struct hio_config_item *item, void *dst)
{
	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		*(int *)dst = item->default_int;
		return 0;
	case HIO_CONFIG_TYPE_FLOAT:
		*(float *)dst = item->default_float;
		return 0;
	case HIO_CONFIG_TYPE_BOOL:
		*(bool *)dst = item->default_bool;
		return 0;
	case HIO_CONFIG_TYPE_ENUM:
		if (item->default_enum < 0 || item->default_enum >= item->max) {
			LOG_ERR("Enum default %d out of range [0, %d) for '%s'", item->default_enum,
				item->max, item->name);
			return -EINVAL;
		}
		memcpy(dst, &item->default_enum, item->size);
		return 0;
	case HIO_CONFIG_TYPE_STRING:
		strncpy(dst, item->default_string, item->size - 1);
		((char *)dst)[item->size - 1] = '\0';
		return 0;
	case HIO_CONFIG_TYPE_HEX:
		memcpy(dst, item->default_hex, item->size);
		return 0;
	}

	return -EINVAL;
}

static int item_init(const struct hio_config_item *item)
{
	return item_default(item, item->variable);
}

static int load_direct_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
			  void *param)

{
	struct hio_config *module = (struct hio_config *)param;

	LOG_DBG("module: %s key: %s", module->name, key);

	int ret;
	const char *next;

	for (int i = 0; i < module->nitems; i++) {
		struct hio_config_item *item = &module->items[i];
		if (settings_name_steq(key, item->name, &next) && !next) {
			if (len != item->size) {
				LOG_WRN("Item '%s' size mismatch: expected %zu, got %zu",
					item->name, item->size, len);
				return 0;
			}
			ret = read_cb(cb_arg, item->variable, len);
			if (ret < 0) {
				LOG_ERR("Call `read_cb` failed: %d", ret);
				return ret;
			}
			return 0;
		}
	}

	return 0;
}

/* Single-writer by design: commits run at registration or before reboot, so
 * the interim->final copy is unlocked. Runtime *_without_reboot commits are
 * the caller's responsibility to synchronize. */
static int commit(const struct hio_config *module)
{
	if (module->commit) {
		int ret = module->commit(module);
		if (ret) {
			return ret;
		}
		LOG_DBG("Committed config for '%s'", module->name);
	} else if (module->interim && module->final && module->size > 0) {
		memcpy(module->final, module->interim, module->size);
		LOG_DBG("Default commit for '%s'", module->name);
	} else {
		LOG_WRN("No commit logic defined for '%s'", module->name);
	}
	return 0;
}

static int register_module(struct hio_config *module, k_timeout_t timeout)
{
	int ret;

	if (module == NULL) {
		LOG_ERR("Invalid module");
		return -EINVAL;
	}

	if (module->name == NULL) {
		LOG_ERR("Module has no name");
		return -EINVAL;
	}

	/*
	 * Wait for the config subsystem to be initialized. By design registration
	 * blocks until hio_config_init() has run, so callers can register from any
	 * init priority. hio_config_register() waits forever; callers that prefer
	 * to fail fast can use hio_config_register_timeout().
	 */
	if (!(k_event_wait(&m_event, CONFIG_EVENT_READY, false, timeout) & CONFIG_EVENT_READY)) {
		LOG_ERR("Config subsystem not initialized while registering '%s'. "
			"Registration must run after hio_config_init(); use a SYS_INIT "
			"priority numerically greater than CONFIG_HIO_CONFIG_INIT_PRIORITY "
			"at APPLICATION level, or hio_config_register() to wait for it.",
			module->name);
		return -ETIMEDOUT;
	}

	/* Check for duplicate */
	struct hio_config *existing;
	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, existing, node) {
		if (strcmp(existing->name, module->name) == 0) {
			LOG_ERR("Config module '%s' is already registered", module->name);
			return -EALREADY;
		}
	}

	for (int i = 0; i < module->nitems; i++) {
		ret = item_init(&module->items[i]);
		if (ret) {
			LOG_WRN("Initializing item '%s' in module '%s' failed: %d",
				module->items[i].name, module->name, ret);
		}
	}

	sys_slist_append(&m_list, &module->node);

	LOG_INF("Config '%s' registered (%d items)", module->name, module->nitems);

	/* Load settings from the storage*/
	if (module->items != NULL && module->nitems > 0) {
		const char *subtree = module->storage_name ? module->storage_name : module->name;
		ret = settings_load_subtree_direct(subtree, load_direct_cb, (void *)module);
		if (ret) {
			LOG_ERR("Could not load module config '%s' (error %d)", module->name, ret);
			return ret;
		}
	}

	ret = commit(module);
	if (ret) {
		LOG_ERR("Commit failed for module '%s': %d", module->name, ret);
		return ret;
	}

	LOG_INF("Loaded stored settings for '%s'", module->name);

	return 0;
}

int hio_config_register(struct hio_config *module)
{
	return register_module(module, K_FOREVER);
}

int hio_config_register_timeout(struct hio_config *module, k_timeout_t timeout)
{
	return register_module(module, timeout);
}

static int save(void)
{
	int ret;

	ret = settings_save();
	if (ret) {
		LOG_ERR("Call `settings_save` failed: %d", ret);
		/* Part of the values may have been stored */
		m_gen++;
		return ret;
	}

	m_saved_gen = m_gen;

	LOG_INF("Settings was saved");

	return 0;
}

int hio_config_save(void)
{
	int ret = save();
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		return ret;
	}

	hio_sys_reboot("Config save");

	return 0;
}

int hio_config_save_without_reboot(void)
{
	return save();
}

#if !defined(CONFIG_SETTINGS_FILE)
static int delete_item_cb(const struct hio_config *module, const struct hio_config_item *item,
			  void *user_data)
{
	if (module == NULL || item == NULL) {
		return -EINVAL;
	}

	if (item->name == NULL) {
		LOG_ERR("Item has no name");
		return -EINVAL;
	}

	char settings_key[CONFIG_HIO_CONFIG_SETTINGS_KEY_MAX];
	snprintf(settings_key, sizeof(settings_key), "%s/%s",
		 module->storage_name ? module->storage_name : module->name, item->name);

	int ret = settings_delete(settings_key);
	if (ret) {
		LOG_ERR("Call `settings_delete` failed: %d", ret);
		return ret;
	}

	return 0;
}
#endif /* !defined(CONFIG_SETTINGS_FILE) */

static int reset(void)
{
	__unused int ret;

	/* The storage goes to the defaults while the values in RAM are kept */
	m_gen++;

#if defined(CONFIG_SETTINGS_FILE)
	/* Settings in external FLASH as a LittleFS file */
	ret = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
	if (ret) {
		LOG_WRN("Call `fs_unlink` failed: %d", ret);
	}

	/* Needs to be static so it is zero-ed */
	static struct fs_file_t file;
	/* Re-initialize so a second reset starts from a defined state */
	fs_file_t_init(&file);
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
	hio_config_iter_items(NULL, delete_item_cb, NULL);
#endif /* defined(CONFIG_SETTINGS_FILE) */

	LOG_INF("Settings was reset");

	return 0;
}

int hio_config_reset(void)
{
	int ret = reset();
	if (ret) {
		LOG_ERR("Call `reset` failed: %d", ret);
		return ret;
	}

	LOG_INF("Reset done");

	hio_sys_reboot("Config reset");

	return 0;
}

int hio_config_reset_without_reboot(void)
{
	return reset();
}

int hio_config_module_iter_items(struct hio_config *module, hio_config_item_cb_t cb,
				 void *user_data)
{
	if (module == NULL || cb == NULL) {
		return -EINVAL;
	}

	if (module->items != NULL && module->nitems > 0) {
		for (int i = 0; i < module->nitems; i++) {
			if (!(hio_config_item_access(module, &module->items[i]) & HIO_CONFIG_ACCESS_SHOW)) {
				continue;
			}
			int ret = cb(module, &module->items[i], user_data);
			if (ret) {
				return ret;
			}
		}
	}

	return 0;
}

static int module_reset(struct hio_config *module)
{
	if (module == NULL) {
		return -EINVAL;
	}

	/* Both RAM and storage go back to the defaults, so a clean config stays
	 * clean unless deleting a stored value fails */
	bool clean = !hio_config_is_dirty();

	m_gen++;

	for (int i = 0; i < module->nitems; i++) {
		if (!(hio_config_item_access(module, &module->items[i]) & HIO_CONFIG_ACCESS_WRITE)) {
			continue;
		}
		if (delete_item_cb(module, &module->items[i], NULL)) {
			clean = false;
		}
		item_init(&module->items[i]);
	}

	if (clean) {
		m_saved_gen = m_gen;
	}

	int ret = commit(module);
	if (ret) {
		LOG_ERR("Commit failed for module '%s': %d", module->name, ret);
		return ret;
	}

	LOG_INF("Module '%s' reset to defaults", module->name);

	return 0;
}

int hio_config_module_reset(struct hio_config *module)
{
	int ret = module_reset(module);
	if (ret) {
		return ret;
	}

	hio_sys_reboot("Module config reset");

	return 0;
}

int hio_config_module_reset_without_reboot(struct hio_config *module)
{
	return module_reset(module);
}

int hio_config_iter_modules(hio_config_module_cb_t cb, void *user_data)
{
	struct hio_config *module;

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, module, node) {
		int ret = cb(module, user_data);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int iter_items_cb(const struct hio_config *module, void *user_data)
{
	void **args = user_data;
	hio_config_item_cb_t cb = args[0];

	return hio_config_module_iter_items((struct hio_config *)module, cb, args[1]);
}

int hio_config_iter_items(const char *filter_module, hio_config_item_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	if (filter_module && filter_module[0] != '\0') {
		struct hio_config *module;
		int ret = hio_config_find_module(filter_module, &module);
		if (ret) {
			return ret;
		}
		return hio_config_module_iter_items(module, cb, user_data);
	}

	void *args[] = {cb, user_data};
	return hio_config_iter_modules(iter_items_cb, args);
}

int hio_config_find_module(const char *name, struct hio_config **module)
{
	if (name == NULL) {
		return -EINVAL;
	}

	struct hio_config *found_module;

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, found_module, node) {
		if (strcmp(found_module->name, name) == 0) {
			if (module) {
				*module = found_module;
			}
			return 0;
		}
	}

	return -ENOENT;
}

int hio_config_module_find_item(struct hio_config *module, const char *name,
				const struct hio_config_item **item)
{

	if (module == NULL || name == NULL) {
		return -EINVAL;
	}

	if (module->items != NULL && module->nitems > 0) {
		for (int i = 0; i < module->nitems; i++) {
			if (strcmp(module->items[i].name, name) == 0) {
				if (item) {
					*item = &module->items[i];
				}
				return 0;
			}
		}
	}

	return -ENOENT;
}

int hio_config_find_item(const char *module_name, const char *name,
			 const struct hio_config_item **item)
{
	if (module_name == NULL || name == NULL) {
		return -EINVAL;
	}

	struct hio_config *module;

	int ret = hio_config_find_module(module_name, &module);
	if (ret) {
		return ret;
	}

	return hio_config_module_find_item(module, name, item);
}

static int parse_int(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	char *endptr;
	long value = strtol(argv, &endptr, 10);
	if (*endptr != '\0') {
		*err_msg = "Invalid format";
		return -EINVAL;
	}

	if (value < item->min || value > item->max) {
		*err_msg = "Invalid range";
		return -EINVAL;
	}

	*((int *)item->variable) = (int)value;

	return 0;
}

static int parse_float(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	char *endptr;
	float value = strtof(argv, &endptr);
	if (*endptr != '\0') {
		*err_msg = "Invalid format";
		return -EINVAL;
	}

	if (value < item->min || value > item->max) {
		*err_msg = "Invalid range";
		return -EINVAL;
	}

	*((float *)item->variable) = value;

	return 0;
}

static int parse_bool(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	bool is_false = !strcmp(argv, "false");
	bool is_true = !strcmp(argv, "true");

	if (is_false) {
		*((bool *)item->variable) = false;
		return 0;
	} else if (is_true) {
		*((bool *)item->variable) = true;
		return 0;
	}
	*err_msg = "Invalid format";
	return -EINVAL;
}

static int parse_enum(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	int value = -1;

	for (size_t i = 0; i < item->max; i++) {
		if (strcmp(argv, item->enums[i]) == 0) {
			value = i;
			break;
		}
	}

	if (value < 0) {
		*err_msg = "Invalid option";
		return -EINVAL;
	}

	memcpy(item->variable, &value, item->size);

	return 0;
}

static int parse_string(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	size_t len = strlen(argv);

	if (item->min > 0 && (int)len < item->min) {
		*err_msg = "Value too short";
		return -EINVAL;
	}

	if (item->max > 0 && (int)len > item->max) {
		*err_msg = "Value too long";
		return -EINVAL;
	}

	if (len + 1 > item->size) {
		*err_msg = "Value too long";
		return -EINVAL;
	}

	strncpy(item->variable, argv, item->size - 1);
	((char *)item->variable)[item->size - 1] = '\0';

	return 0;
}

static int parse_hex(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	int ret = hio_hex2buf(argv, item->variable, item->size, true);

	if (ret != item->size) {
		*err_msg = "Length does not match";
		return -EINVAL;
	}

	return 0;
}

int hio_config_module_item_set_value(const struct hio_config *module, const struct hio_config_item *item,
			   char *argv, const char **err_msg)
{
	if (!(hio_config_item_access(module, item) & HIO_CONFIG_ACCESS_WRITE)) {
		*err_msg = "Read-only";
		return -EPERM;
	}

	int ret = -EINVAL;

	if (item->parse_cb != NULL) {
		ret = item->parse_cb(item, argv, err_msg);
	} else {
		switch (item->type) {
		case HIO_CONFIG_TYPE_INT:
			ret = parse_int(item, argv, err_msg);
			break;
		case HIO_CONFIG_TYPE_FLOAT:
			ret = parse_float(item, argv, err_msg);
			break;
		case HIO_CONFIG_TYPE_BOOL:
			ret = parse_bool(item, argv, err_msg);
			break;
		case HIO_CONFIG_TYPE_ENUM:
			ret = parse_enum(item, argv, err_msg);
			break;
		case HIO_CONFIG_TYPE_STRING:
			ret = parse_string(item, argv, err_msg);
			break;
		case HIO_CONFIG_TYPE_HEX:
			ret = parse_hex(item, argv, err_msg);
			break;
		}
	}

	if (ret == 0) {
		m_gen++;
	}

	return ret;
}

int hio_config_module_item_store(const struct hio_config *module,
				 const struct hio_config_item *item, const void *value)
{
	char key[CONFIG_HIO_CONFIG_SETTINGS_KEY_MAX];

	if (module == NULL || item == NULL || value == NULL) {
		return -EINVAL;
	}

	snprintf(key, sizeof(key), "%s/%s",
		 module->storage_name ? module->storage_name : module->name, item->name);

	int ret = settings_save_one(key, value, item->size);
	if (ret) {
		LOG_ERR("Call `settings_save_one` failed (%s): %d", key, ret);
		return ret;
	}

	/* RAM and storage change together, so a clean config stays clean */
	bool clean = !hio_config_is_dirty();

	memcpy(item->variable, value, item->size);

	m_gen++;
	if (clean) {
		m_saved_gen = m_gen;
	}

	return 0;
}

uint32_t hio_config_get_gen(void)
{
	return m_gen;
}

bool hio_config_is_dirty(void)
{
	return m_gen != m_saved_gen;
}

int hio_config_item_format_line(const struct hio_config *module,
				const struct hio_config_item *item, char *buf, size_t size)
{
	return hio_config_item_format_line_value(module, item, item->variable, buf, size);
}

static int write_str(hio_config_write_cb_t cb, void *user_data, const char *str)
{
	return cb(str, strlen(str), user_data);
}

/* Write the value part of a "show" line */
static int write_value(const struct hio_config_item *item, const void *value,
		       hio_config_write_cb_t cb, void *user_data)
{
	/* Fits any float with "%.2f" */
	char tmp[48];
	int ret;

	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		snprintf(tmp, sizeof(tmp), "%d", *(const int *)value);
		return write_str(cb, user_data, tmp);

	case HIO_CONFIG_TYPE_FLOAT:
		snprintf(tmp, sizeof(tmp), "%.2f", (double)*(const float *)value);
		return write_str(cb, user_data, tmp);

	case HIO_CONFIG_TYPE_BOOL:
		return write_str(cb, user_data, *(const bool *)value ? "true" : "false");

	case HIO_CONFIG_TYPE_ENUM: {
		int32_t val = 0;
		memcpy(&val, value, item->size);

		if (val < 0 || val >= item->max) {
			snprintf(tmp, sizeof(tmp), "<invalid: %d>", val);
			return write_str(cb, user_data, tmp);
		}

		if ((ret = write_str(cb, user_data, "\"")) ||
		    (ret = write_str(cb, user_data, item->enums[val]))) {
			return ret;
		}
		return write_str(cb, user_data, "\"");
	}

	case HIO_CONFIG_TYPE_STRING:
		if ((ret = write_str(cb, user_data, "\"")) ||
		    (ret = cb(value, strnlen(value, item->size), user_data))) {
			return ret;
		}
		return write_str(cb, user_data, "\"");

	case HIO_CONFIG_TYPE_HEX:
		for (size_t i = 0; i < item->size;) {
			size_t n = 0;

			for (; i < item->size && n + 2 < sizeof(tmp); i++) {
				n += snprintf(tmp + n, sizeof(tmp) - n, "%02x",
					      ((const uint8_t *)value)[i]);
			}
			ret = cb(tmp, n, user_data);
			if (ret) {
				return ret;
			}
		}
		return 0;
	}

	return -EINVAL;
}

int hio_config_item_write_line(const struct hio_config *module,
			       const struct hio_config_item *item, const void *value,
			       hio_config_write_cb_t cb, void *user_data)
{
	int ret;

	if ((ret = write_str(cb, user_data, module->name)) ||
	    (ret = write_str(cb, user_data, " config ")) ||
	    (ret = write_str(cb, user_data, item->name)) ||
	    (ret = write_str(cb, user_data, " "))) {
		return ret;
	}

	return write_value(item, value, cb, user_data);
}

struct format_line_ctx {
	char *buf;
	size_t size;
	size_t len;
};

static int format_line_cb(const void *data, size_t len, void *user_data)
{
	struct format_line_ctx *ctx = user_data;

	/* Keep room for the terminator */
	if (len >= ctx->size - ctx->len) {
		return -ENOSPC;
	}

	memcpy(ctx->buf + ctx->len, data, len);
	ctx->len += len;

	return 0;
}

int hio_config_item_format_line_value(const struct hio_config *module,
				      const struct hio_config_item *item, const void *value,
				      char *buf, size_t size)
{
	if (size == 0) {
		return -ENOSPC;
	}

	struct format_line_ctx ctx = {.buf = buf, .size = size};

	int ret = hio_config_item_write_line(module, item, value, format_line_cb, &ctx);
	if (ret) {
		return ret;
	}

	buf[ctx.len] = '\0';

	return ctx.len;
}

/* Load the saved value of the item into dst, or its default if it is not stored */
static int item_load_saved(const struct hio_config *module, const struct hio_config_item *item,
			   void *dst)
{
	char key[CONFIG_HIO_CONFIG_SETTINGS_KEY_MAX];

	snprintf(key, sizeof(key), "%s/%s",
		 module->storage_name ? module->storage_name : module->name, item->name);

	ssize_t len = settings_load_one(key, dst, item->size);
	if (len < 0) {
		LOG_ERR("Call `settings_load_one` failed (%s): %d", key, (int)len);
		return len;
	}

	/* Missing or size-mismatched values are not loaded on boot either */
	if (len != item->size) {
		return item_default(item, dst);
	}

	return 0;
}

static bool item_value_equal(const struct hio_config_item *item, const void *a, const void *b)
{
	if (item->type == HIO_CONFIG_TYPE_STRING) {
		return strncmp(a, b, item->size) == 0;
	}

	return memcmp(a, b, item->size) == 0;
}

struct iter_saved_ctx {
	hio_config_value_cb_t cb;
	void *user_data;
	bool diff_only;
};

static int iter_saved_cb(const struct hio_config *module, const struct hio_config_item *item,
			 void *user_data)
{
	struct iter_saved_ctx *ctx = user_data;
	uint8_t value[CONFIG_HIO_CONFIG_VALUE_MAX_SIZE] __aligned(sizeof(void *));
	int ret;

	if (item->size > sizeof(value)) {
		LOG_ERR("Item '%s' in module '%s' is larger than %zu bytes", item->name,
			module->name, sizeof(value));
		return -ENOSPC;
	}

	ret = item_load_saved(module, item, value);
	if (ret) {
		return ret;
	}

	if (ctx->diff_only && item_value_equal(item, item->variable, value)) {
		return 0;
	}

	return ctx->cb(module, item, value, ctx->user_data);
}

int hio_config_iter_saved(hio_config_value_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	struct iter_saved_ctx ctx = {.cb = cb, .user_data = user_data};

	return hio_config_iter_items(NULL, iter_saved_cb, &ctx);
}

int hio_config_iter_diff(hio_config_value_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	if (!hio_config_is_dirty()) {
		return 0;
	}

	struct iter_saved_ctx ctx = {.cb = cb, .user_data = user_data, .diff_only = true};

	return hio_config_iter_items(NULL, iter_saved_cb, &ctx);
}

int hio_config_parse_line(char *line, const char **err_msg)
{
	const char *dummy;

	if (err_msg == NULL) {
		err_msg = &dummy;
	}
	*err_msg = NULL;

	char *saveptr;
	char *module_name = strtok_r(line, " ", &saveptr);
	char *keyword = strtok_r(NULL, " ", &saveptr);
	char *item_name = strtok_r(NULL, " ", &saveptr);
	char *value = strtok_r(NULL, "", &saveptr);

	if (module_name == NULL || keyword == NULL || strcmp(keyword, "config") != 0 ||
	    item_name == NULL || value == NULL) {
		*err_msg = "Malformed config line";
		return -EINVAL;
	}

	/* Strip one pair of surrounding double quotes, matching what the Zephyr
	 * shell tokenizer does before a value reaches argv. */
	size_t vlen = strlen(value);
	if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
		value[vlen - 1] = '\0';
		value++;
	}

	int ret;
	struct hio_config *module;

	ret = hio_config_find_module(module_name, &module);
	if (ret) {
		*err_msg = "Unknown module";
		return ret;
	}

	const struct hio_config_item *item;

	ret = hio_config_module_find_item(module, item_name, &item);
	if (ret) {
		*err_msg = "Unknown parameter";
		return ret;
	}

	return hio_config_module_item_set_value(module, item, value, err_msg);
}

static int iter_commit_cb(const struct hio_config *module, void *user_data)
{
	int ret = commit(module);
	if (ret) {
		LOG_ERR("Commit failed for '%s': %d", module->name, ret);
	}
	/* Intentionally do not propagate: a faulty module's commit failure must
	 * not abort iteration and block the remaining subsystems' commits. */
	return 0;
}

static int h_commit(void)
{
	LOG_DBG("Committing settings");

	return hio_config_iter_modules(iter_commit_cb, NULL);
}

static int h_export_cb(const struct hio_config *module, const struct hio_config_item *item,
		       void *user_data)
{
	char name_concat[CONFIG_HIO_CONFIG_SETTINGS_KEY_MAX];
	const char *subtree = module->storage_name ? module->storage_name : module->name;

	snprintf(name_concat, sizeof(name_concat), "%s/%s", subtree, item->name);

	int (*export_func)(const char *, const void *, size_t) = user_data;

	return export_func(name_concat, item->variable, item->size);
}

static int h_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	LOG_DBG("Exporting settings");

	return hio_config_iter_items(NULL, h_export_cb, export_func);
}

/* clang-format on */
static int hio_config_init(void)
{
	int ret;

	LOG_INF("Initializing HIO config system");

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("settings_subsys_init() failed: %d", ret);
		return ret;
	}

	static struct settings_handler sh = {
		.name = CONFIG_HIO_CONFIG_SETTINGS_PFX, /*  default prefix is "" */
		.h_commit = h_commit,
		.h_export = h_export,
	};

	ret = settings_register(&sh);
	if (ret) {
		LOG_ERR("settings_register() failed: %d", ret);
		return ret;
	}

	k_event_post(&m_event, CONFIG_EVENT_READY);

	LOG_INF("HIO config system initialized successfully");

	return 0;
}

SYS_INIT(hio_config_init, APPLICATION, CONFIG_HIO_CONFIG_INIT_PRIORITY);
