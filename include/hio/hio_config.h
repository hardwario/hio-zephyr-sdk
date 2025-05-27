#ifndef HIO_INCLUDE_HIO_CONFIG_H_
#define HIO_INCLUDE_HIO_CONFIG_H_

/* Zephyr includes */
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>

/* Standard include */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_config hio_config
 * @{
 */

enum hio_config_item_type {
	HIO_CONFIG_TYPE_INT,
	HIO_CONFIG_TYPE_FLOAT,
	HIO_CONFIG_TYPE_BOOL,
	HIO_CONFIG_TYPE_ENUM,
	HIO_CONFIG_TYPE_STRING,
	HIO_CONFIG_TYPE_HEX,
};

#define HIO_CONFIG_ITEM_INT(_name_d, _var, _min, _max, _help, _default)                            \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_INT,                                                       \
		.variable = &_var,                                                                 \
		.size = sizeof(_var),                                                              \
		.min = _min,                                                                       \
		.max = _max,                                                                       \
		.help = _help,                                                                     \
		.default_int = _default,                                                           \
	}

#define HIO_CONFIG_ITEM_FLOAT(_name_d, _var, _min, _max, _help, _default)                          \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_FLOAT,                                                     \
		.variable = &_var,                                                                 \
		.size = sizeof(_var),                                                              \
		.min = _min,                                                                       \
		.max = _max,                                                                       \
		.help = _help,                                                                     \
		.default_float = _default,                                                         \
	}

#define HIO_CONFIG_ITEM_BOOL(_name_d, _var, _help, _default)                                       \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_BOOL,                                                      \
		.variable = &_var,                                                                 \
		.size = sizeof(_var),                                                              \
		.help = _help,                                                                     \
		.default_bool = _default,                                                          \
	}

#define HIO_CONFIG_ITEM_ENUM(_name_d, _var, _items_str, _help, _default)                           \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_ENUM,                                                      \
		.variable = &_var,                                                                 \
		.size = sizeof(_var),                                                              \
		.min = 0,                                                                          \
		.max = ARRAY_SIZE(_items_str),                                                     \
		.help = _help,                                                                     \
		.enums = _items_str,                                                               \
		.default_enum = _default,                                                          \
	}

#define HIO_CONFIG_ITEM_STRING(_name_d, _var, _help, _default)                                     \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_STRING,                                                    \
		.variable = _var,                                                                  \
		.size = ARRAY_SIZE(_var),                                                          \
		.help = _help,                                                                     \
		.default_string = _default,                                                        \
	}

#define HIO_CONFIG_ITEM_STRING_PARSE_CB(_name_d, _var, _help, _default, _cb)                       \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_STRING,                                                    \
		.variable = _var,                                                                  \
		.size = ARRAY_SIZE(_var),                                                          \
		.help = _help,                                                                     \
		.default_string = _default,                                                        \
		.parse_cb = _cb,                                                                   \
	}

#define HIO_CONFIG_ITEM_HEX(_name_d, _var, _help, _default)                                        \
	{                                                                                          \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_HEX,                                                       \
		.variable = _var,                                                                  \
		.size = ARRAY_SIZE(_var),                                                          \
		.help = _help,                                                                     \
		.default_hex = _default,                                                           \
	}

struct hio_config_item;

typedef int (*hio_config_parse_cb)(const struct hio_config_item *item, char *argv,
				   const char **err_msg);

struct hio_config_item {
	const char *name;
	enum hio_config_item_type type;
	void *variable;
	size_t size;
	int min;
	int max;
	const char *help;
	const char **enums;
	union {
		int default_int;
		float default_float;
		bool default_bool;
		int default_enum;
		const char *default_string;
		const uint8_t *default_hex;
	};
	hio_config_parse_cb parse_cb;
};
struct hio_config {
	const char *name;              /**< Module name (used as prefix for settings) */
	const char *storage_name;      /**< Optional name used as prefix for settings storage */
	struct hio_config_item *items; /**< Configuration items */
	int nitems;                    /**< Number of configuration items */

	void *interim; /**< Temporary config structure */
	void *final;   /**< Final config structure */
	size_t size;   /**< Size of the config structure */

	int (*commit)(
		const struct hio_config *module); /**< Commit function to override default commit */

	sys_snode_t node;
};

/**
 * @brief Register a configuration module.
 *
 * This function registers a configuration module with the given name and
 * items. The items are used to define the configuration parameters for the
 * module.
 *
 * @param config Pointer to the configuration module to register.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_config_register(struct hio_config *module);
/**
 * @brief Save configuration to persistent storage and reboot system.
 */
int hio_config_save(void);

/**
 * @brief Save configuration to persistent storage without rebooting.
 */
int hio_config_save_without_reboot(void);

/**
 * @brief Reset configuration to defaults and reboot system.
 */
int hio_config_reset(void);

/**
 * @brief Reset configuration to defaults without rebooting.
 */
int hio_config_reset_without_reboot(void);

/**
 * @brief Callback for iterating over registered modules.
 *
 * @param module Pointer to the registered module (struct hio_config).
 * @param user_data User-defined context pointer.
 *
 * @return 0 to continue iteration, non-zero to stop early.
 */
typedef int (*hio_config_module_cb_t)(const struct hio_config *module, void *user_data);

/**
 * @brief Iterate over all registered configuration modules.
 *
 * Calls the provided callback for each registered module.
 *
 * @param cb         Callback function to call per module.
 * @param user_data  Pointer passed to the callback.
 *
 * @return 0 on success or first non-zero value returned by callback.
 */
int hio_config_iter_modules(hio_config_module_cb_t cb, void *user_data);

/**
 * @brief Callback for iterating over config items.
 *
 * @param module      Pointer to the registered module (struct hio_config).
 * @param item        Pointer to the configuration item.
 * @param user_data   User-defined context.
 *
 * @return 0 to continue iteration, non-zero to stop early.
 */
typedef int (*hio_config_item_cb_t)(const struct hio_config *module,
				    const struct hio_config_item *item, void *user_data);

/**
 * @brief Iterate over all configuration items across modules.
 *
 * If @p filter_module is NULL or empty, all modules are included.
 * Otherwise, only items from the specified module are passed to the callback.
 *
 * @param filter_module Optional module name to filter by.
 * @param cb            Callback called for each item, receives module name.
 * @param user_data     User context pointer.
 *
 * @return 0 on success or first non-zero value returned by callback.
 */
int hio_config_iter_items(const char *filter_module, hio_config_item_cb_t cb, void *user_data);

/**
 * @brief Find a module by name.
 *
 * Searches for a registered module by its name.
 *
 * @param name Name of the module to find.
 * @param module Pointer to store the found module.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_config_find_module(const char *name, struct hio_config **module);

/**
 * @brief Find a configuration item by name.
 *
 * Searches for a specific configuration item within a module.
 *
 * @param module_name Name of the module.
 * @param name        Name of the item to find.
 * @param item        Pointer to store the found item.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_config_find_item(const char *module_name, const char *name,
			 const struct hio_config_item **item);

/**
 * @brief Find a configuration item within a module
 *
 * Searches for a specific configuration item by its name within the given module.
 *
 * @param module Pointer to the configuration module.
 * @param name Name of the item to find.
 * @param item Pointer to store the found item.
 *
 * @return 0 on success, negative error code on failure.
 *
 */
int hio_config_module_find_item(struct hio_config *module, const char *name,
				const struct hio_config_item **item);

/**
 * @brief  Parse a configuration item.
 *
 * Parses the value of a configuration item from a string.
 *
 * @param item Pointer to the configuration item.
 * @param argv Pointer to the string containing the value.
 * @param err_msg Pointer to store error message if parsing fails.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_config_item_parse(const struct hio_config_item *item, char *argv, const char **err_msg);

#define HIO_CONFIG_SHELL_CMD_ARG                                                                   \
	SHELL_CMD_ARG(config, NULL, "Configuration commands.", hio_config_shell_cmd, 1, 3)

/**
 * @brief Shell command to manage configuration.
 *
 * @param shell Pointer to the shell instance.
 * @param argc Number of arguments passed to the command.
 * @param argv Array of argument strings.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_config_shell_cmd(const struct shell *shell, size_t argc, char **argv);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_CONFIG_H_ */
