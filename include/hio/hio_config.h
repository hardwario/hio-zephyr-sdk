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
		.module = SETTINGS_PFX,                                                            \
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
		.module = SETTINGS_PFX,                                                            \
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
		.module = SETTINGS_PFX,                                                            \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_BOOL,                                                      \
		.variable = &_var,                                                                 \
		.size = sizeof(_var),                                                              \
		.help = _help,                                                                     \
		.default_bool = _default,                                                          \
	}

#define HIO_CONFIG_ITEM_ENUM(_name_d, _var, _items_str, _help, _default)                           \
	{                                                                                          \
		.module = SETTINGS_PFX,                                                            \
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
		.module = SETTINGS_PFX,                                                            \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_STRING,                                                    \
		.variable = _var,                                                                  \
		.size = ARRAY_SIZE(_var),                                                          \
		.help = _help,                                                                     \
		.default_string = _default,                                                        \
	}

#define HIO_CONFIG_ITEM_STRING_PARSE_CB(_name_d, _var, _help, _default, _cb)                       \
	{                                                                                          \
		.module = SETTINGS_PFX,                                                            \
		.name = _name_d,                                                                   \
		.type = CTR_CONFIG_TYPE_STRING,                                                    \
		.variable = _var,                                                                  \
		.size = ARRAY_SIZE(_var),                                                          \
		.help = _help,                                                                     \
		.default_string = _default,                                                        \
		.parse_cb = _cb,                                                                   \
	}

#define HIO_CONFIG_ITEM_HEX(_name_d, _var, _help, _default)                                        \
	{                                                                                          \
		.module = SETTINGS_PFX,                                                            \
		.name = _name_d,                                                                   \
		.type = HIO_CONFIG_TYPE_HEX,                                                       \
		.variable = _var,                                                                  \
		.size = ARRAY_SIZE(_var),                                                          \
		.help = _help,                                                                     \
		.default_hex = _default,                                                           \
	}

struct hio_config_item;

typedef int (*hio_config_parse_cb)(const struct shell *shell, char *argv,
	const struct hio_config_item *item);

struct hio_config_item {
	const char *module;
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

typedef int (*hio_config_show_cb)(const struct shell *shell, size_t argc, char **argv);

int hio_config_save(bool reboot);
int hio_config_reset(bool reboot);
void hio_config_append_show(const char *name, hio_config_show_cb cb);

int hio_config_show_item(const struct shell *shell, const struct hio_config_item *item);
int hio_config_help_item(const struct shell *shell, const struct hio_config_item *item);
int hio_config_parse_item(const struct shell *shell, char *argv,
			  const struct hio_config_item *item);
int hio_config_init_item(const struct hio_config_item *item);

int hio_config_cmd_config(const struct hio_config_item *items, int nitems,
			  const struct shell *shell, size_t argc, char **argv);
int hio_config_h_export(const struct hio_config_item *items, int nitems,
			int (*export_func)(const char *name, const void *val, size_t val_len));
int hio_config_h_set(const struct hio_config_item *items, int nitems, const char *key, size_t len,
		     settings_read_cb read_cb, void *cb_arg);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_CONFIG_H_ */
