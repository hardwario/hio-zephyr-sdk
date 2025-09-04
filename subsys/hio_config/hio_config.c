/* HIO includes */
#include <hio/hio_config.h>
#include <hio/hio_util.h>
#include <hio/hio_sys.h>

/* Zephyr includes */
#include <zephyr/fs/fs.h>
#include <zephyr/fs/nvs.h>
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

#define SETTINGS_PFX ""

LOG_MODULE_REGISTER(hio_config, CONFIG_HIO_CONFIG_LOG_LEVEL);

#define CONFIG_EVENT_READY BIT(0)
static K_EVENT_DEFINE(m_event);
static sys_slist_t m_list = SYS_SLIST_STATIC_INIT(&m_list);

static int item_init(const struct hio_config_item *item)
{
	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		*(int *)item->variable = item->default_int;
		return 0;
	case HIO_CONFIG_TYPE_FLOAT:
		*(float *)item->variable = item->default_float;
		return 0;
	case HIO_CONFIG_TYPE_BOOL:
		*(bool *)item->variable = item->default_bool;
		return 0;
	case HIO_CONFIG_TYPE_ENUM:
		memcpy(item->variable, &item->default_enum, item->size);
		return 0;
	case HIO_CONFIG_TYPE_STRING:
		strcpy(item->variable, item->default_string);
		return 0;
	case HIO_CONFIG_TYPE_HEX:
		memcpy(item->variable, item->default_hex, item->size);
		return 0;
	}

	return -EINVAL;
}

static int load_direct_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
			  void *param)

{
	struct hio_config *module = (struct hio_config *)param;

	LOG_DBG("moudule: %s key: %s", module->name, key);

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

int hio_config_register(struct hio_config *module)
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

	k_event_wait(&m_event, CONFIG_EVENT_READY, false, K_FOREVER);

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

static int save(void)
{
	int ret;

	ret = settings_save();
	if (ret) {
		LOG_ERR("Call `settings_save` failed: %d", ret);
		return ret;
	}

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

static int delete_item_cb(const struct hio_config *module, const struct hio_config_item *item,
			  void *user_data)
{
	if (module == NULL || item == NULL) {
		return -EINVAL;
	}

	if (item->name == NULL) {
		LOG_ERR("Item '%s' has no name", item->name);
		return -EINVAL;
	}

	char settings_key[50];
	snprintf(settings_key, sizeof(settings_key), "%s/%s",
		 module->storage_name ? module->storage_name : module->name, item->name);

	int ret = settings_delete(settings_key);
	if (ret) {
		LOG_ERR("Call `settings_delete` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int reset(void)
{
	__unused int ret;

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

int hio_config_iter_items(const char *filter_module, hio_config_item_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	struct hio_config *module;

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, module, node) {
		if (filter_module && filter_module[0] != '\0') {
			if (strcmp(module->name, filter_module) != 0) {
				continue;
			}
		}

		if (module->items != NULL && module->nitems > 0) {
			for (int i = 0; i < module->nitems; i++) {
				int ret = cb(module, &module->items[i], user_data);
				if (ret) {
					return ret;
				}
			}
		}

		if (filter_module && filter_module[0] != '\0') {
			break; /* if found */
		}
	}

	return 0;
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
	float value;
	int ret = sscanf(argv, "%f", &value);

	if (ret != 1) {
		*err_msg = "Invalid value";
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
	if (strlen(argv) + 1 > item->size) {
		*err_msg = "Value too long";
		return -EINVAL;
	}

	strcpy(item->variable, argv);

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

int hio_config_item_parse(const struct hio_config_item *item, char *argv, const char **err_msg)
{
	if (item->parse_cb != NULL) {
		return item->parse_cb(item, argv, err_msg);
	}

	switch (item->type) {
	case HIO_CONFIG_TYPE_INT:
		return parse_int(item, argv, err_msg);
	case HIO_CONFIG_TYPE_FLOAT:
		return parse_float(item, argv, err_msg);
	case HIO_CONFIG_TYPE_BOOL:
		return parse_bool(item, argv, err_msg);
	case HIO_CONFIG_TYPE_ENUM:
		return parse_enum(item, argv, err_msg);
	case HIO_CONFIG_TYPE_STRING:
		return parse_string(item, argv, err_msg);
	case HIO_CONFIG_TYPE_HEX:
		return parse_hex(item, argv, err_msg);
	}

	return -EINVAL;
}

static int iter_commit_cb(const struct hio_config *module, void *user_data)
{
	int ret = commit(module);
	if (ret) {
		LOG_ERR("Commit failed for '%s': %d", module->name, ret);
	}
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
	char name_concat[64];
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
