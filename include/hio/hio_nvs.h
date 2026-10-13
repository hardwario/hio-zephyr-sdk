/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_INCLUDE_HIO_NVS_H_
#define HIO_INCLUDE_HIO_NVS_H_

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_nvs hio_nvs
 * @{
 *
 * Persistent runtime data (counters, timestamps, accumulators) kept in a
 * dedicated flash partition `hio_nvs_storage`, separate from the settings
 * partition used by hio_config.
 *
 * Each module registers a table of items pointing to its own variables. On
 * registration the stored values are loaded into the variables (a variable
 * without a stored value is left untouched, so it may be preset with a
 * default before registering). All items are then written back periodically
 * (CONFIG_HIO_NVS_SAVE_INTERVAL) and right before a hio_sys_reboot(). The
 * underlying NVS skips writes of unchanged values, so an unchanged item costs
 * no flash wear.
 *
 * Item names are global across all modules (they identify the record in
 * flash and are used by the `nvs` shell command) - prefix them with the
 * module name, e.g. `bat_energy_0`.
 *
 * Variables are read by the save work from the system work queue. Updates
 * that must be stored consistently (e.g. a 64-bit value, or several items
 * that change together) must be done under hio_nvs_lock().
 */

enum hio_nvs_item_type {
	HIO_NVS_TYPE_INT32,
	HIO_NVS_TYPE_UINT32,
	HIO_NVS_TYPE_INT64,
	HIO_NVS_TYPE_FLOAT,
};

#define HIO_NVS_ITEM_(_name, _type, _ctype, _var)                                                  \
	{                                                                                          \
		.name = _name, .type = _type, .variable = &_var,                                   \
		.size = sizeof(_var) + ZERO_OR_COMPILE_ERROR(sizeof(_var) == sizeof(_ctype)),      \
	}

#define HIO_NVS_ITEM_INT32(_name, _var)  HIO_NVS_ITEM_(_name, HIO_NVS_TYPE_INT32, int32_t, _var)
#define HIO_NVS_ITEM_UINT32(_name, _var) HIO_NVS_ITEM_(_name, HIO_NVS_TYPE_UINT32, uint32_t, _var)
#define HIO_NVS_ITEM_INT64(_name, _var)  HIO_NVS_ITEM_(_name, HIO_NVS_TYPE_INT64, int64_t, _var)
#define HIO_NVS_ITEM_FLOAT(_name, _var)  HIO_NVS_ITEM_(_name, HIO_NVS_TYPE_FLOAT, float, _var)

struct hio_nvs_item {
	const char *name;            /**< Item name (globally unique, identifies the record) */
	enum hio_nvs_item_type type; /**< Value type */
	void *variable;              /**< Pointer to the backing variable */
	size_t size;                 /**< Size of the variable in bytes */
	uint16_t id;                 /**< NVS record ID (assigned on registration) */
};

struct hio_nvs {
	const char *name;           /**< Module name (logs, `nvs show <module>`) */
	struct hio_nvs_item *items; /**< Persistent items */
	int nitems;                 /**< Number of items */

	sys_snode_t node;
};

/**
 * @brief Register a module and load its stored values.
 *
 * Waits (forever) for the NVS subsystem to be initialized, so it may be
 * called from any init priority.
 *
 * @param module Module to register (must have static lifetime).
 *
 * @return 0 on success, -EALREADY if a module of the same name is registered,
 *         -EEXIST if an item name (or its record ID) clashes with an already
 *         registered item, other negative error code on failure.
 */
int hio_nvs_register(struct hio_nvs *module);

/**
 * @brief Lock the item variables against the save work.
 *
 * Recursive; pair every call with hio_nvs_unlock().
 */
void hio_nvs_lock(void);

/**
 * @brief Unlock the item variables.
 */
void hio_nvs_unlock(void);

/**
 * @brief Write all items to flash now.
 *
 * Unchanged values are not rewritten. Does not postpone the periodic save.
 *
 * @return 0 on success, negative error code of the first failed write.
 */
int hio_nvs_save(void);

/**
 * @brief Write a single item to flash now.
 *
 * Intended for rare events that must not be lost (e.g. a battery exchange).
 *
 * @param name Item name.
 *
 * @return 0 on success, -ENOENT if no such item, negative error code on failure.
 */
int hio_nvs_save_item(const char *name);

/**
 * @brief Find an item by name.
 *
 * @param name   Item name.
 * @param module Optional output for the owning module.
 * @param item   Optional output for the item.
 *
 * @return 0 on success, -ENOENT if not found.
 */
int hio_nvs_find_item(const char *name, const struct hio_nvs **module,
		      const struct hio_nvs_item **item);

/**
 * @brief Callback for iterating over items.
 *
 * @return 0 to continue iteration, non-zero to stop early.
 */
typedef int (*hio_nvs_item_cb_t)(const struct hio_nvs *module, const struct hio_nvs_item *item,
				 void *user_data);

/**
 * @brief Iterate over items of all modules (or of a single one).
 *
 * @param filter_module Module name, or NULL / empty for all modules.
 * @param cb            Callback called for each item.
 * @param user_data     User context pointer.
 *
 * @return 0 on success, -ENOENT if @p filter_module is not registered, or the
 *         first non-zero value returned by @p cb.
 */
int hio_nvs_iter_items(const char *filter_module, hio_nvs_item_cb_t cb, void *user_data);

/**
 * @brief Parse a value from a string, set the variable and save the item.
 *
 * @param item  Item.
 * @param value Value string (decimal; FLOAT accepts any strtof() format).
 *
 * @return 0 on success, -EINVAL on a malformed or out-of-range value,
 *         negative error code on a failed write.
 */
int hio_nvs_item_set(const struct hio_nvs_item *item, const char *value);

/**
 * @brief Delete the stored item and zero its variable.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_nvs_item_reset(const struct hio_nvs_item *item);

/**
 * @brief Format the item value as a string.
 *
 * @return Length of the string on success, -ENOSPC if the buffer is too small.
 */
int hio_nvs_item_format(const struct hio_nvs_item *item, char *buf, size_t size);

/**
 * @brief Erase the whole NVS partition and zero all registered variables.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_nvs_erase(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_NVS_H_ */
