/* HIO includes */
// #include <hio/hio_log.h>
#include <hio/hio_config.h>

/* Zephyr includes */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct hio_log_config {
#if defined(CONFIG_HIO_LOG_DBG)
	char debug[64 + 1];
#endif
#if defined(CONFIG_HIO_LOG_INF)
	char info[64 + 1];
#endif
#if defined(CONFIG_HIO_LOG_WRN)
	char warn[64 + 1];
#endif
#if defined(CONFIG_HIO_LOG_ERR)
	char error[64 + 1];
#endif
};

struct hio_log_config g_hio_log_config;
static struct hio_log_config m_config_interim;

LOG_MODULE_REGISTER(hio_log, CONFIG_HIO_LOG_LOG_LEVEL);

/* clang-format off */

static struct hio_config_item m_config_items[] = {

	#if defined(CONFIG_HIO_LOG_DBG)
	HIO_CONFIG_ITEM_STRING("debug", m_config_interim.debug, "Comma separated list of module names", CONFIG_HIO_LOG_DBG_DEFAULT),
	#endif

	#if defined(CONFIG_HIO_LOG_INF)
	HIO_CONFIG_ITEM_STRING("info", m_config_interim.info, "Comma separated list of module names", CONFIG_HIO_LOG_INF_DEFAULT),
	#endif

	#if defined(CONFIG_HIO_LOG_WRN)
	HIO_CONFIG_ITEM_STRING("warn", m_config_interim.warn, "Comma separated list of module names", CONFIG_HIO_LOG_WRN_DEFAULT),
	#endif

	#if defined(CONFIG_HIO_LOG_ERR)
	HIO_CONFIG_ITEM_STRING("error", m_config_interim.error, "Comma separated list of module names", CONFIG_HIO_LOG_ERR_DEFAULT),
	#endif
};
/* clang-format on */

static int apply_rules(void)
{
	struct {
		const char *level_name;
		const char *modules;
		uint32_t log_level;
	} log_levels[] = {

#if defined(CONFIG_HIO_LOG_INF)
		{"INF", g_hio_log_config.info, LOG_LEVEL_INF},
#endif

#if defined(CONFIG_HIO_LOG_DBG)
		{"DBG", g_hio_log_config.debug, LOG_LEVEL_DBG},
#endif

#if defined(CONFIG_HIO_LOG_WRN)
		{"WRN", g_hio_log_config.warn, LOG_LEVEL_WRN},
#endif

#if defined(CONFIG_HIO_LOG_ERR)
		{"ERR", g_hio_log_config.error, LOG_LEVEL_ERR},
#endif

	};

	uint32_t numLogSources = log_src_cnt_get(0);

	for (size_t i = 0; i < ARRAY_SIZE(log_levels); i++) {
		for (uint32_t sourceId = 0; sourceId < numLogSources; sourceId++) {
			char *sourceName = (char *)log_source_name_get(0, sourceId);
			// TODO improve so that hio_atci is not enabled when module is hio_at.
			if (strstr(log_levels[i].modules, sourceName)) {
				LOG_INF("Enabling %s for %s", log_levels[i].level_name, sourceName);
				log_filter_set(NULL, 0, sourceId, log_levels[i].log_level);
				// log_frontend_filter_set(sourceId, log_levels[i].log_level);
			}
		}
	}
	return 0;
}

static int init(void)
{
	LOG_INF("Initializing HIO log subsystem");

	static struct hio_config config = {
		.name = "log",
		.items = m_config_items,
		.nitems = ARRAY_SIZE(m_config_items),

		.interim = &m_config_interim,
		.final = &g_hio_log_config,
		.size = sizeof(g_hio_log_config),
	};

	hio_config_register(&config);
	apply_rules();

	return 0;
}

static int cmd_list(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	uint32_t numLogSources = log_src_cnt_get(0);
	for (uint32_t sourceId = 0; sourceId < numLogSources; sourceId++) {
		char *sourceName = (char *)log_source_name_get(0, sourceId);
		shell_print(shell, "%s", sourceName);
	}

	shell_info(shell, "command succeeded");

	return 0;
}

static int cmd_apply(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	apply_rules();

	shell_info(shell, "command succeeded");

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
	sub_log,

	HIO_CONFIG_SHELL_CMD_ARG,

	SHELL_CMD_ARG(list, NULL, "List all log backends.", cmd_list, 1, 0),
	SHELL_CMD_ARG(apply, NULL, "Apply log configuration again.", cmd_apply, 1, 0),

	SHELL_SUBCMD_SET_END);

/* clang-format on */

SHELL_CMD_REGISTER(log, &sub_log, "Log commands.", print_help);

// Load after hio config
BUILD_ASSERT(CONFIG_HIO_CONFIG_INIT_PRIORITY < CONFIG_HIO_LOG_INIT_PRIORITY);

SYS_INIT(init, APPLICATION, CONFIG_HIO_LOG_INIT_PRIORITY);
