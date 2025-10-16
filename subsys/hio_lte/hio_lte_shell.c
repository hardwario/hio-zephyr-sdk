#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_state.h"
#include "hio_lte_talk.h"

/* HIO includes */
#include <hio/hio_lte.h>
#include <hio/hio_config.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

LOG_MODULE_REGISTER(hio_lte_shell, CONFIG_HIO_LTE_LOG_LEVEL);

static int cmd_imei(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	uint64_t imei;
	ret = hio_lte_get_imei(&imei);
	if (ret) {
		LOG_ERR("Call `hio_lte_get_imei` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	shell_print(shell, "imei: %llu", imei);

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_imsi(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	uint64_t imsi;
	ret = hio_lte_get_imsi(&imsi);
	if (ret) {
		LOG_ERR("Call `hio_lte_get_imsi` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	shell_print(shell, "imsi: %llu", imsi);

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_iccid(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	char *iccid;
	ret = hio_lte_get_iccid(&iccid);
	if (ret) {
		LOG_ERR("Call `hio_lte_get_iccid` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	shell_print(shell, "iccid: %s", iccid);

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_fw_version(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	char *version;
	ret = hio_lte_get_modem_fw_version(&version);
	if (ret) {
		LOG_ERR("Call `hio_lte_get_modem_fw_version` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	shell_print(shell, "fw-version: %s", version);

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_state(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	bool attached;
	ret = hio_lte_is_attached(&attached);
	if (ret) {
		LOG_ERR("Call `hio_lte_is_attached` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	shell_print(shell, "attached: %s", attached ? "yes" : "no");

	const char *lte_cereg = "not available";
	const char *lte_mode = "not available";
	struct hio_lte_cereg_param cereg_param;
	hio_lte_get_cereg_param(&cereg_param);
	if (cereg_param.valid) {
		lte_cereg = hio_lte_str_cereg_stat(cereg_param.stat);
		lte_mode = hio_lte_str_act(cereg_param.act);
	}
	shell_print(shell, "cereg: %s", lte_cereg);
	shell_print(shell, "mode: %s", lte_mode);

	struct hio_lte_conn_param conn_param;
	hio_lte_get_conn_param(&conn_param);
	if (conn_param.valid) {
		shell_print(shell, "eest: %d", conn_param.eest);
		shell_print(shell, "ecl: %d", conn_param.ecl);
		shell_print(shell, "rsrp: %d", conn_param.rsrp);
		shell_print(shell, "rsrq: %d", conn_param.rsrq);
		shell_print(shell, "snr: %d", conn_param.snr);
		shell_print(shell, "plmn: %d", conn_param.plmn);
		shell_print(shell, "cid: %d", conn_param.cid);
		shell_print(shell, "band: %d", conn_param.band);
		shell_print(shell, "earfcn: %d", conn_param.earfcn);
	}

	shell_print(shell, "fsm-state: %s", hio_lte_get_state());

	if (strcmp(hio_lte_get_state(), "attach") == 0) {
		int attempt, attach_timeout_sec, remaining_sec;
		if (!hio_lte_get_curr_attach_info(&attempt, &attach_timeout_sec, NULL,
						  &remaining_sec)) {
			shell_print(shell, "attach-attempt: %d", attempt);
			shell_print(shell, "attach-timeout: %d:%02d", attach_timeout_sec / 60,
				    attach_timeout_sec % 60);
			shell_print(shell, "attach-remaining: %d:%02d", remaining_sec / 60,
				    remaining_sec % 60);
		}
	} else if (strcmp(hio_lte_get_state(), "retry_delay") == 0) {
		int attempt, retry_delay_sec, remaining_sec;
		if (!hio_lte_get_curr_attach_info(&attempt, NULL, &retry_delay_sec,
						  &remaining_sec)) {
			shell_print(shell, "attach-attempt: %d", attempt);
			shell_print(shell, "retry-delay-timeout: %d:%02d", retry_delay_sec / 60,
				    retry_delay_sec % 60);
			shell_print(shell, "retry-delay-remaining: %d:%02d", remaining_sec / 60,
				    remaining_sec % 60);
		}
	}

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_metrics(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	struct hio_lte_metrics metrics;
	ret = hio_lte_get_metrics(&metrics);
	if (ret) {
		shell_error(shell, "hio_lte_get_metrics failed: %d", ret);
		return ret;
	}

	shell_print(shell, "uplink messages: %u", metrics.uplink_count);
	shell_print(shell, "uplink bytes: %u", metrics.uplink_bytes);
	shell_print(shell, "uplink errors: %u", metrics.uplink_errors);
	shell_print(shell, "uplink last ts: %lld", metrics.uplink_last_ts);
	shell_print(shell, "downlink messages: %u", metrics.downlink_count);
	shell_print(shell, "downlink bytes: %u", metrics.downlink_bytes);
	shell_print(shell, "downlink errors: %u", metrics.downlink_errors);
	shell_print(shell, "downlink last ts: %lld", metrics.downlink_last_ts);

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_test_modem(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 2) {
		shell_error(shell, "command not found: %s", argv[2]);
		shell_help(shell);
		return -EINVAL;
	}

	if (!g_hio_lte_config.test) {
		shell_error(shell, "test mode is not activated");
		return -ENOEXEC;
	}

	if (strlen(argv[1]) == 5 && strncmp(argv[1], "start", 5) == 0) {
		ret = hio_lte_flow_start();
		if (ret) {
			LOG_ERR("Call `hio_lte_flow_start` failed: %d", ret);
			shell_error(shell, "command failed");
			return ret;
		}

		shell_info(shell, "command succeeded");

		return 0;
	}

	if (strlen(argv[1]) == 4 && strncmp(argv[1], "stop", 4) == 0) {
		ret = hio_lte_flow_stop();
		if (ret) {
			LOG_ERR("Call `hio_lte_flow_stop` failed: %d", ret);
			shell_error(shell, "command failed");
			return ret;
		}

		shell_info(shell, "command succeeded");

		return 0;
	}

	shell_help(shell);
	return -EINVAL;
}

static int cmd_test_cmd(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 2) {
		shell_error(shell, "only one argument is accepted (use quotes?)");
		shell_help(shell);
		return -EINVAL;
	}

	if (!g_hio_lte_config.test) {
		shell_error(shell, "test mode is not activated");
		return -ENOEXEC;
	}

	ret = hio_lte_flow_cmd(argv[1]);
	if (ret) {
		if (ret == -ENOTCONN) {
			shell_warn(shell, "modem is not connected");
			return 0;
		}
		LOG_ERR("Call `hio_lte_flow_cmd` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	return 0;
}

static int cmd_test_prepare(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	if (!g_hio_lte_config.test) {
		shell_error(shell, "test mode is not activated");
		return -ENOEXEC;
	}

	ret = hio_lte_flow_prepare();
	if (ret) {
		LOG_ERR("Call `hio_lte_flow_prepare` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	shell_info(shell, "command succeeded");

	return 0;
}

static void flow_bypass_cb(void *user_data, const uint8_t *data, size_t len)
{
	// shell_print((const struct shell *)user_data, "%s", (const char *)data);
	const struct shell_fprintf *fprintf_ctx = (struct shell_fprintf *)user_data;
	fprintf_ctx->fwrite(fprintf_ctx->user_ctx, data, len);
}

static void shell_bypass_cb(const struct shell *shell, uint8_t *data, size_t len)
{
	static char line[256];
	static size_t line_len = 0;

	if (len == 0) {
		return;
	}

	if (strncmp((const char *)data, "+++", 3) == 0) {
		shell_print(shell, "exiting bypass mode");
		hio_lte_talk_bypass_set_cb(NULL, NULL);
		shell_set_bypass(shell, NULL);
		return;
	}

	for (size_t i = 0; i < len; i++) {
		if (data[i] == '\r' || data[i] == '\n') {
			if (line_len == 0) {
				continue; // skip empty lines
			}
			line[line_len] = '\0';
			hio_lte_talk_at_cmd(line);
			line_len = 0;
			continue;
		}

		line[line_len++] = data[i];
		if (line_len >= sizeof(line)) {
			line_len = 0;
		}
	}
}

static int cmd_test_bypass(const struct shell *shell, size_t argc, char **argv)
{

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	if (!g_hio_lte_config.test) {
		shell_error(shell, "test mode is not activated");
		return -ENOEXEC;
	}

	hio_lte_talk_bypass_set_cb(flow_bypass_cb, (void *)shell->fprintf_ctx);
	shell_set_bypass(shell, shell_bypass_cb);

	shell_print(shell, "bypass mode enabled, for exit type +++");

	return 0;
}

int cmd_test_modemtrace(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	int lvl = 0;
	if (argc != 2) {
		shell_error(shell, "only one argument is accepted (use quotes?)");
		shell_help(shell);
		return -EINVAL;
	} else if (argc == 2) {
		lvl = atoi(argv[1]);
		if (lvl < 0 || lvl > 5) {
			shell_error(shell, "invalid trace level: %d", lvl);
			return -EINVAL;
		}
	}

	ret = hio_lte_flow_xmodemtrace(lvl);
	if (ret) {
		LOG_ERR("Call `hio_lte_flow_xmodemtrace` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int cmd_reconnect(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (g_hio_lte_config.test) {
		shell_error(shell, "not supported in test mode");
		return -ENOEXEC;
	}

	ret = hio_lte_reconnect();
	if (ret) {
		LOG_ERR("Call `hio_lte_reconnect` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

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
	sub_lte_test,

	SHELL_CMD_ARG(modem, NULL,
	              "Start/stop modem library (format: <start|stop>).",
	              cmd_test_modem, 2, 0),

	SHELL_CMD_ARG(cmd, NULL,
	              "Send command to modem. (format: <command>)",
	              cmd_test_cmd, 2, 0),

	SHELL_CMD_ARG(prepare, NULL,
	              "Run prepare modem sequence.",
		      cmd_test_prepare, 1, 0),

	SHELL_CMD_ARG(bypass, NULL, "Switch to bypass mode.", cmd_test_bypass, 1, 1),

	SHELL_CMD_ARG(modemtrace, NULL,
	              "Set modem trace level (format: <0-5>).",
	              cmd_test_modemtrace, 2, 0),

	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_lte,

	HIO_CONFIG_SHELL_CMD_ARG,

	SHELL_CMD_ARG(imei, NULL,
	              "Get modem IMEI.",
	              cmd_imei, 1, 0),

	SHELL_CMD_ARG(imsi, NULL,
	              "Get SIM card IMSI.",
	              cmd_imsi, 1, 0),

	SHELL_CMD_ARG(iccid, NULL,
		      "Get SIM card ICCID.",
	              cmd_iccid, 1, 0),

	SHELL_CMD_ARG(fw-version, NULL,
	              "Get modem firmware version.",
	              cmd_fw_version, 1, 0),

	SHELL_CMD_ARG(state, NULL,
	              "Get LTE state.",
	              cmd_state, 1, 0),

	SHELL_CMD_ARG(metrics, NULL,
		     "Get LTE metrics.",
	              cmd_metrics, 1, 0),

	SHELL_CMD_ARG(test, &sub_lte_test,
	              "Test commands.",
	              print_help, 1, 0),

	SHELL_CMD_ARG(reconnect, NULL,
	              "Reconnect LTE modem.",
	              cmd_reconnect, 1, 0),


	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(lte, &sub_lte, "LTE commands.", print_help);
