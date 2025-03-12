#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_state.h"

/* HIO includes */
#include <hio/hio_lte.h>

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

	struct hio_lte_cereg_param cereg_param;
	hio_lte_get_cereg_param(&cereg_param);
	if (cereg_param.valid) {
		switch (cereg_param.stat) {
		case HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED:
			shell_print(shell, "cereg: not registered");
			break;
		case HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME:
			shell_print(shell, "cereg: registered home");
			break;
		case HIO_LTE_CEREG_PARAM_STAT_SEARCHING:
			shell_print(shell, "cereg: searching");
			break;
		case HIO_LTE_CEREG_PARAM_STAT_REGISTRATION_DENIED:
			shell_print(shell, "cereg: registration denied");
			break;
		case HIO_LTE_CEREG_PARAM_STAT_UNKNOWN:
			shell_print(shell, "cereg: unknown");
			break;
		case HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING:
			shell_print(shell, "cereg: registered roaming");
			break;
		case HIO_LTE_CEREG_PARAM_STAT_SIM_FAILURE:
			shell_print(shell, "cereg: sim failure");
			break;
		default:
			shell_print(shell, "cereg: unknown");
			break;
		}

		switch (cereg_param.act) {
		case HIO_LTE_CEREG_PARAM_ACT_LTE:
			shell_print(shell, "mode: lte-m");
			break;
		case HIO_LTE_CEREG_PARAM_ACT_NBIOT:
			shell_print(shell, "mode: nb-iot");
			break;
		case HIO_LTE_CEREG_PARAM_ACT_UNKNOWN:
		default:
			shell_print(shell, "act: unknown");
			break;
		}
	}

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

	shell_print(shell, "state: %s", hio_lte_get_state());

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
		LOG_ERR("Call `hio_lte_flow_cmd_without_response` failed: %d", ret);
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

	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_lte,

	SHELL_CMD_ARG(config, NULL,
	              "Configuration commands.",
	              hio_lte_config_cmd, 1, 3),

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

	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(lte, &sub_lte, "LTE commands.", print_help);
