/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_nvs.h>
#include <hio/hio_sys.h>

/* Zephyr includes */
#include <zephyr/drivers/flash.h>
/* NVS moved from zephyr/fs to zephyr/kvss in Zephyr 4.3 (NCS 3.4) */
#if __has_include(<zephyr/kvss/nvs.h>)
#include <zephyr/kvss/nvs.h>
#else
#include <zephyr/fs/nvs.h>
#endif
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/slist.h>

/* Standard includes */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_nvs, CONFIG_HIO_NVS_LOG_LEVEL);

#if !FIXED_PARTITION_EXISTS(hio_nvs_storage)
#error "CONFIG_HIO_NVS needs a `hio_nvs_storage` flash partition (pm_static.yml or devicetree)"
#endif

#define NVS_PARTITION_ID FIXED_PARTITION_ID(hio_nvs_storage)

/* Largest item variable (INT64) */
#define ITEM_MAX_SIZE 8

/* ID 0xFFFF is reserved by NVS for its close/GC-done entries */
#define ID_RESERVED 0xFFFF

#define EVENT_READY BIT(0)

static K_EVENT_DEFINE(m_event);
static sys_slist_t m_list = SYS_SLIST_STATIC_INIT(&m_list);

/* Serializes NVS operations (and erase/remount) and guards the module list */
static K_MUTEX_DEFINE(m_fs_lock);
/* Guards the item variables (public hio_nvs_lock()) */
static K_MUTEX_DEFINE(m_var_lock);

static struct nvs_fs m_fs;
static bool m_mounted;

static void save_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_save_work, save_work_handler);

void hio_nvs_lock(void)
{
	k_mutex_lock(&m_var_lock, K_FOREVER);
}

void hio_nvs_unlock(void)
{
	k_mutex_unlock(&m_var_lock);
}

static uint16_t item_id(const char *name)
{
	uint16_t id = crc16_ccitt(0xFFFF, (const uint8_t *)name, strlen(name));

	return id == ID_RESERVED ? 0 : id;
}

/* Caller holds m_fs_lock */
static int item_save(const struct hio_nvs_item *item)
{
	uint8_t buf[ITEM_MAX_SIZE];

	if (!m_mounted) {
		return -ENODEV;
	}

	/* Snapshot under the variable lock, write without holding it - a write
	 * may trigger a sector erase and the application must not wait for it */
	hio_nvs_lock();
	memcpy(buf, item->variable, item->size);
	hio_nvs_unlock();

	ssize_t ret = nvs_write(&m_fs, item->id, buf, item->size);
	if (ret < 0) {
		LOG_ERR("Call `nvs_write` failed for '%s': %d", item->name, (int)ret);
		return ret;
	}

	if (ret > 0) {
		LOG_DBG("Saved '%s'", item->name);
	}

	return 0;
}

/* Caller holds m_fs_lock */
static int save_all(void)
{
	int err = 0;
	struct hio_nvs *module;

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, module, node) {
		for (int i = 0; i < module->nitems; i++) {
			int ret = item_save(&module->items[i]);
			if (ret && !err) {
				err = ret;
			}
		}
	}

	return err;
}

int hio_nvs_save(void)
{
	k_mutex_lock(&m_fs_lock, K_FOREVER);
	int ret = save_all();
	k_mutex_unlock(&m_fs_lock);

	return ret;
}

int hio_nvs_save_item(const char *name)
{
	const struct hio_nvs_item *item;

	int ret = hio_nvs_find_item(name, NULL, &item);
	if (ret) {
		return ret;
	}

	k_mutex_lock(&m_fs_lock, K_FOREVER);
	ret = item_save(item);
	k_mutex_unlock(&m_fs_lock);

	return ret;
}

static void save_work_handler(struct k_work *work)
{
	k_work_reschedule(&m_save_work, K_SECONDS(CONFIG_HIO_NVS_SAVE_INTERVAL));

	LOG_DBG("Periodic save");

	hio_nvs_save();
}

static void reboot_notifier_cb(const char *reason, void *user_data)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(user_data);

	LOG_INF("Saving before reboot");

	hio_nvs_save();
}

/* Caller holds m_fs_lock */
static int check_clash(const struct hio_nvs_item *new_item)
{
	struct hio_nvs *module;

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, module, node) {
		for (int i = 0; i < module->nitems; i++) {
			const struct hio_nvs_item *item = &module->items[i];

			if (strcmp(item->name, new_item->name) == 0) {
				LOG_ERR("Item '%s' already registered by '%s'", new_item->name,
					module->name);
				return -EEXIST;
			}

			if (item->id == new_item->id) {
				LOG_ERR("Item '%s' ID 0x%04x clashes with '%s' of '%s', rename it",
					new_item->name, new_item->id, item->name, module->name);
				return -EEXIST;
			}
		}
	}

	return 0;
}

/* Caller holds m_fs_lock */
static void item_load(const struct hio_nvs_item *item)
{
	uint8_t buf[ITEM_MAX_SIZE];

	ssize_t ret = nvs_read(&m_fs, item->id, buf, sizeof(buf));
	if (ret == -ENOENT) {
		LOG_DBG("No stored value for '%s'", item->name);
		return;
	}

	if (ret < 0) {
		LOG_ERR("Call `nvs_read` failed for '%s': %d", item->name, (int)ret);
		return;
	}

	if (ret != item->size) {
		LOG_WRN("Item '%s' size mismatch: expected %zu, got %d", item->name, item->size,
			(int)ret);
		return;
	}

	hio_nvs_lock();
	memcpy(item->variable, buf, item->size);
	hio_nvs_unlock();
}

int hio_nvs_register(struct hio_nvs *module)
{
	int ret;

	if (module == NULL || module->name == NULL || (module->nitems > 0 && !module->items)) {
		LOG_ERR("Invalid module");
		return -EINVAL;
	}

	k_event_wait(&m_event, EVENT_READY, false, K_FOREVER);

	if (!m_mounted) {
		LOG_ERR("NVS not available, module '%s' not registered", module->name);
		return -ENODEV;
	}

	k_mutex_lock(&m_fs_lock, K_FOREVER);

	struct hio_nvs *existing;
	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, existing, node) {
		if (strcmp(existing->name, module->name) == 0) {
			LOG_ERR("Module '%s' already registered", module->name);
			ret = -EALREADY;
			goto unlock;
		}
	}

	for (int i = 0; i < module->nitems; i++) {
		struct hio_nvs_item *item = &module->items[i];

		if (item->name == NULL || item->size == 0 || item->size > ITEM_MAX_SIZE) {
			LOG_ERR("Invalid item %d in module '%s'", i, module->name);
			ret = -EINVAL;
			goto unlock;
		}

		item->id = item_id(item->name);

		ret = check_clash(item);
		if (ret) {
			goto unlock;
		}

		/* Clashes within the module itself */
		for (int j = 0; j < i; j++) {
			if (module->items[j].id == item->id) {
				LOG_ERR("Item '%s' clashes with '%s' in module '%s'", item->name,
					module->items[j].name, module->name);
				ret = -EEXIST;
				goto unlock;
			}
		}
	}

	for (int i = 0; i < module->nitems; i++) {
		item_load(&module->items[i]);
	}

	sys_slist_append(&m_list, &module->node);

	LOG_INF("Module '%s' registered (%d items)", module->name, module->nitems);

	ret = 0;

unlock:
	k_mutex_unlock(&m_fs_lock);

	return ret;
}

int hio_nvs_find_item(const char *name, const struct hio_nvs **module,
		      const struct hio_nvs_item **item)
{
	struct hio_nvs *m;

	if (name == NULL) {
		return -EINVAL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, m, node) {
		for (int i = 0; i < m->nitems; i++) {
			if (strcmp(m->items[i].name, name) == 0) {
				if (module) {
					*module = m;
				}
				if (item) {
					*item = &m->items[i];
				}
				return 0;
			}
		}
	}

	return -ENOENT;
}

int hio_nvs_iter_items(const char *filter_module, hio_nvs_item_cb_t cb, void *user_data)
{
	struct hio_nvs *module;
	bool found = false;

	if (cb == NULL) {
		return -EINVAL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, module, node) {
		if (filter_module && filter_module[0] != '\0' &&
		    strcmp(module->name, filter_module) != 0) {
			continue;
		}

		found = true;

		for (int i = 0; i < module->nitems; i++) {
			int ret = cb(module, &module->items[i], user_data);
			if (ret) {
				return ret;
			}
		}
	}

	if (filter_module && filter_module[0] != '\0' && !found) {
		return -ENOENT;
	}

	return 0;
}

int hio_nvs_item_set(const struct hio_nvs_item *item, const char *value)
{
	char *end;
	uint8_t buf[ITEM_MAX_SIZE];

	if (item == NULL || value == NULL || value[0] == '\0') {
		return -EINVAL;
	}

	errno = 0;

	switch (item->type) {
	case HIO_NVS_TYPE_INT32: {
		long long v = strtoll(value, &end, 10);
		if (v < INT32_MIN || v > INT32_MAX) {
			return -EINVAL;
		}
		int32_t v32 = v;
		memcpy(buf, &v32, sizeof(v32));
		break;
	}
	case HIO_NVS_TYPE_UINT32: {
		long long v = strtoll(value, &end, 10);
		if (v < 0 || v > UINT32_MAX) {
			return -EINVAL;
		}
		uint32_t v32 = v;
		memcpy(buf, &v32, sizeof(v32));
		break;
	}
	case HIO_NVS_TYPE_INT64: {
		int64_t v = strtoll(value, &end, 10);
		memcpy(buf, &v, sizeof(v));
		break;
	}
	case HIO_NVS_TYPE_FLOAT: {
		float v = strtof(value, &end);
		memcpy(buf, &v, sizeof(v));
		break;
	}
	default:
		return -EINVAL;
	}

	if (*end != '\0' || errno == ERANGE) {
		return -EINVAL;
	}

	hio_nvs_lock();
	memcpy(item->variable, buf, item->size);
	hio_nvs_unlock();

	k_mutex_lock(&m_fs_lock, K_FOREVER);
	int ret = item_save(item);
	k_mutex_unlock(&m_fs_lock);

	return ret;
}

int hio_nvs_item_reset(const struct hio_nvs_item *item)
{
	int ret;

	if (item == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&m_fs_lock, K_FOREVER);

	if (!m_mounted) {
		ret = -ENODEV;
		goto unlock;
	}

	ret = nvs_delete(&m_fs, item->id);
	if (ret) {
		LOG_ERR("Call `nvs_delete` failed for '%s': %d", item->name, ret);
		goto unlock;
	}

	hio_nvs_lock();
	memset(item->variable, 0, item->size);
	hio_nvs_unlock();

unlock:
	k_mutex_unlock(&m_fs_lock);

	return ret;
}

int hio_nvs_item_format(const struct hio_nvs_item *item, char *buf, size_t size)
{
	int len;

	hio_nvs_lock();

	switch (item->type) {
	case HIO_NVS_TYPE_INT32:
		len = snprintf(buf, size, "%" PRId32, *(int32_t *)item->variable);
		break;
	case HIO_NVS_TYPE_UINT32:
		len = snprintf(buf, size, "%" PRIu32, *(uint32_t *)item->variable);
		break;
	case HIO_NVS_TYPE_INT64:
		len = snprintf(buf, size, "%lld", (long long)*(int64_t *)item->variable);
		break;
	case HIO_NVS_TYPE_FLOAT:
		len = snprintf(buf, size, "%.3f", (double)*(float *)item->variable);
		break;
	default:
		len = -EINVAL;
		break;
	}

	hio_nvs_unlock();

	if (len >= 0 && (size_t)len >= size) {
		return -ENOSPC;
	}

	return len;
}

/* Caller holds m_fs_lock (or runs before anything else can use NVS) */
static int mount(void)
{
	int ret;
	const struct flash_area *fa;
	struct flash_sector sector;
	uint32_t sector_cnt = 1;

	m_mounted = false;

	ret = flash_area_open(NVS_PARTITION_ID, &fa);
	if (ret) {
		LOG_ERR("Call `flash_area_open` failed: %d", ret);
		return ret;
	}

	ret = flash_area_get_sectors(NVS_PARTITION_ID, &sector_cnt, &sector);
	if (ret && ret != -ENOMEM) {
		LOG_ERR("Call `flash_area_get_sectors` failed: %d", ret);
		goto close;
	}

	if (sector.fs_size == 0 || sector.fs_size > UINT16_MAX ||
	    fa->fa_size / sector.fs_size < 2) {
		LOG_ERR("Unsupported partition layout (size: %zu, sector: %zu)", fa->fa_size,
			sector.fs_size);
		ret = -EINVAL;
		goto close;
	}

	m_fs.flash_device = fa->fa_dev;
	m_fs.offset = fa->fa_off;
	m_fs.sector_size = sector.fs_size;
	m_fs.sector_count = fa->fa_size / sector.fs_size;

	ret = nvs_mount(&m_fs);
	if (ret) {
		LOG_WRN("Call `nvs_mount` failed: %d, erasing partition", ret);

		ret = flash_area_erase(fa, 0, fa->fa_size);
		if (ret) {
			LOG_ERR("Call `flash_area_erase` failed: %d", ret);
			goto close;
		}

		ret = nvs_mount(&m_fs);
		if (ret) {
			LOG_ERR("Call `nvs_mount` failed: %d", ret);
			goto close;
		}
	}

	m_mounted = true;

	LOG_INF("Mounted (%u x %u B, free: %d B)", m_fs.sector_count, m_fs.sector_size,
		(int)nvs_calc_free_space(&m_fs));

close:
	flash_area_close(fa);

	return ret;
}

int hio_nvs_erase(void)
{
	int ret;
	struct hio_nvs *module;

	k_mutex_lock(&m_fs_lock, K_FOREVER);

	if (m_mounted) {
		ret = nvs_clear(&m_fs);
		if (ret) {
			LOG_ERR("Call `nvs_clear` failed: %d", ret);
			goto unlock;
		}
	}

	ret = mount();
	if (ret) {
		goto unlock;
	}

	hio_nvs_lock();
	SYS_SLIST_FOR_EACH_CONTAINER(&m_list, module, node) {
		for (int i = 0; i < module->nitems; i++) {
			memset(module->items[i].variable, 0, module->items[i].size);
		}
	}
	hio_nvs_unlock();

	LOG_INF("Partition erased");

unlock:
	k_mutex_unlock(&m_fs_lock);

	return ret;
}

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

	ret = mount();
	if (ret) {
		/* Release waiting registrations, they fail with -ENODEV */
		k_event_post(&m_event, EVENT_READY);
		return ret;
	}

	static struct hio_sys_reboot_notifier reboot_notifier = {
		.cb = reboot_notifier_cb,
	};

	ret = hio_sys_add_reboot_notifier(&reboot_notifier);
	if (ret) {
		LOG_ERR("Call `hio_sys_add_reboot_notifier` failed: %d", ret);
	}

	/* Postpone the first save, so a boot loop does not wear the flash */
	k_work_reschedule(&m_save_work, K_SECONDS(MIN(CONFIG_HIO_NVS_FIRST_SAVE_DELAY,
						      CONFIG_HIO_NVS_SAVE_INTERVAL)));

	k_event_post(&m_event, EVENT_READY);

	return 0;
}

SYS_INIT(init, APPLICATION, CONFIG_HIO_NVS_INIT_PRIORITY);
