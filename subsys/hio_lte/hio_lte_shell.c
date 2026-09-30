#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_parse.h"
#include "hio_lte_state.h"
#include "hio_lte_talk.h"

/* HIO includes */
#include <hio/hio_lte.h>
#include <hio/hio_config.h>
#include <hio/hio_util.h>

/* Nordic includes */
#include <ncs_version.h>

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

static void print_cereg_event(const struct shell *shell, const char *label,
			      const struct hio_lte_cereg_event *e)
{
	char ago[24];

	hio_util_fmt_duration(k_uptime_seconds() - e->uptime_s, ago, sizeof(ago));

	if (e->reject_cause) {
		shell_print(shell,
			    "%s%s ago, %s, act: %s, plmn: %u, tac: %04X, cell: %08X, emm: %u (%s)",
			    label, ago, hio_lte_str_cereg_stat(e->stat), hio_lte_str_act(e->act),
			    e->plmn, e->tac, e->cid, e->reject_cause,
			    hio_lte_str_emm_cause(e->reject_cause));
	} else {
		shell_print(shell, "%s%s ago, %s, act: %s, plmn: %u, tac: %04X, cell: %08X", label,
			    ago, hio_lte_str_cereg_stat(e->stat), hio_lte_str_act(e->act), e->plmn,
			    e->tac, e->cid);
	}
}

static void print_last_reject(const struct shell *shell)
{
	struct hio_lte_cereg_event e;

	if (hio_lte_get_last_reject(&e)) {
		return;
	}

	print_cereg_event(shell, "last-reject: ", &e);

	const char *hint = hio_lte_str_emm_cause_hint(e.reject_cause);
	if (hint) {
		shell_print(shell, "last-reject-hint: %s", hint);
	}
}

static int cmd_history(const struct shell *shell, size_t argc, char **argv)
{
	static struct hio_lte_cereg_event events[CONFIG_HIO_LTE_CEREG_HISTORY];
	size_t count;

	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	int ret = hio_lte_get_cereg_history(events, ARRAY_SIZE(events), &count);
	if (ret) {
		shell_error(shell, "command failed");
		return ret;
	}

	/* Oldest first, the newest ends up next to the prompt. */
	for (size_t i = count; i > 0; i--) {
		print_cereg_event(shell, "", &events[i - 1]);
	}

	print_last_reject(shell);

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

	char *ceer;
	if (!hio_lte_get_ceer(&ceer)) {
		shell_print(shell, "last-ceer: %s", ceer);
	}

	print_last_reject(shell);

	const char *fsm_state = "not available";
	hio_lte_get_fsm_state(&fsm_state);

	shell_print(shell, "fsm-state: %s", fsm_state);

	if (strcmp(fsm_state, "attach") == 0) {
		int attempt, attach_timeout_sec, remaining_sec;
		if (!hio_lte_get_curr_attach_info(&attempt, &attach_timeout_sec, NULL,
						  &remaining_sec)) {
			shell_print(shell, "attach-attempt: %d", attempt);
			shell_print(shell, "attach-timeout: %d:%02d", attach_timeout_sec / 60,
				    attach_timeout_sec % 60);
			shell_print(shell, "attach-remaining: %d:%02d", remaining_sec / 60,
				    remaining_sec % 60);
		}
	} else if (strcmp(fsm_state, "retry_delay") == 0) {
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

	struct hio_lte_ncellmeas_param ncellmeas_param;
	hio_lte_get_ncellmeas_param(&ncellmeas_param);
	if (ncellmeas_param.valid) {
		if (ncellmeas_param.status != 0) {
			shell_print(shell, "ncellmeas-error: status %u", ncellmeas_param.status);
		}
		shell_print(shell, "ncellmeas-act: %s", hio_lte_str_act(ncellmeas_param.act));
		shell_print(shell, "ncellmeas-num-cells: %u", ncellmeas_param.num_cells);
		shell_print(shell, "ncellmeas-num-ncells: %u", ncellmeas_param.num_ncells);
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

	shell_print(shell, "attach attempts: %u", metrics.attach_count);
	shell_print(shell, "attach fails: %u", metrics.attach_fail_count);
	shell_print(shell, "attach last duration: %u ms", metrics.attach_last_duration_ms);
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

static const char *cops_stat_str(int stat)
{
	switch (stat) {
	case 1:
		return "available";
	case 2:
		return "current";
	case 3:
		return "forbidden";
	default:
		return "unknown";
	}
}

static const char *cops_act_str(int act)
{
	switch (act) {
	case 7:
		return "lte-m";
	case 9:
		return "nb-iot";
	default:
		return "unknown";
	}
}

static const char *scan_status_str(int status)
{
	switch (status) {
	case 0:
		return "ok";
	case -ENODATA:
		return "not run";
	case -EINTR:
		return "partial";
	case -EBUSY:
		return "radio busy";
	case -ECANCELED:
		return "cancelled";
	default:
		return "error";
	}
}

/* nRF91x1 index to dBm ("reported value of RSRP - 141"). */
static int rsrp_dbm(int16_t index)
{
	return index - 141;
}

struct scan_plmn {
	uint16_t mcc;
	uint16_t mnc;
	const char *str; /* From the network list, NULL if only cells have it. */
};

static void scan_plmn_add(struct scan_plmn *list, size_t *n, size_t max, uint16_t mcc,
			  uint16_t mnc, const char *str)
{
	for (size_t i = 0; i < *n; i++) {
		if (list[i].mcc == mcc && list[i].mnc == mnc) {
			if (!list[i].str) {
				list[i].str = str;
			}
			return;
		}
	}

	if (*n < max) {
		list[(*n)++] = (struct scan_plmn){.mcc = mcc, .mnc = mnc, .str = str};
	}
}

static bool scan_entry_plmn(const struct hio_lte_scan_entry *e, uint16_t *mcc, uint16_t *mnc)
{
	int16_t c, n;

	if (hio_lte_parse_plmn(e->plmn, NULL, &c, &n)) {
		return false;
	}
	*mcc = c;
	*mnc = n;
	return true;
}

/* Stronger first within a RAT; LTE-M before NB-IoT. */
static bool scan_cell_before(const struct hio_lte_scan_cell *a, const struct hio_lte_scan_cell *b)
{
	return a->act != b->act ? a->act < b->act : a->rsrp > b->rsrp;
}

static void print_scan_plmn(const struct shell *shell, const struct hio_lte_scan_result *r,
			    size_t entries, const struct scan_plmn *p)
{
	int stat_ltem = -1;
	int stat_nbiot = -1;
	bool ltem = false;
	bool nbiot = false;
	uint16_t mcc, mnc;

	for (size_t i = 0; i < entries; i++) {
		const struct hio_lte_scan_entry *e = &r->entries[i];

		if (!scan_entry_plmn(e, &mcc, &mnc) || mcc != p->mcc || mnc != p->mnc) {
			continue;
		}
		if (e->act == HIO_LTE_CEREG_PARAM_ACT_NBIOT) {
			stat_nbiot = e->stat;
			nbiot = true;
		} else {
			stat_ltem = e->stat;
			ltem = true;
		}
	}

	uint8_t cells[ARRAY_SIZE(r->cells)];
	size_t n = 0;

	for (size_t i = 0; i < r->cell_count; i++) {
		const struct hio_lte_scan_cell *c = &r->cells[i];

		if (c->mcc != p->mcc || c->mnc != p->mnc) {
			continue;
		}
		if (c->act == HIO_LTE_CEREG_PARAM_ACT_NBIOT) {
			nbiot = true;
		} else {
			ltem = true;
		}

		size_t j = n++;
		for (; j > 0 && scan_cell_before(c, &r->cells[cells[j - 1]]); j--) {
			cells[j] = cells[j - 1];
		}
		cells[j] = i;
	}

	char plmn[12];
	char stat[48];

	if (p->str) {
		snprintf(plmn, sizeof(plmn), "%s", p->str);
	} else {
		snprintf(plmn, sizeof(plmn), "%03u%02u", p->mcc, p->mnc);
	}

	if (stat_ltem < 0 && stat_nbiot < 0) {
		snprintf(stat, sizeof(stat), "not in network list");
	} else if (stat_ltem < 0 || stat_nbiot < 0 || stat_ltem == stat_nbiot) {
		snprintf(stat, sizeof(stat), "%s",
			 cops_stat_str(stat_ltem >= 0 ? stat_ltem : stat_nbiot));
	} else {
		snprintf(stat, sizeof(stat), "%s (lte-m), %s (nb-iot)", cops_stat_str(stat_ltem),
			 cops_stat_str(stat_nbiot));
	}

	shell_print(shell, "plmn: %s, act: %s%s%s, stat: %s", plmn, ltem ? "lte-m" : "",
		    ltem && nbiot ? "," : "", nbiot ? "nb-iot" : "", stat);

	for (size_t i = 0; i < n; i++) {
		const struct hio_lte_scan_cell *c = &r->cells[cells[i]];

		shell_print(shell,
			    "  cell: %08X, act: %s, tac: %04X, earfcn: %u, pci: %u, rsrp: %d dBm",
			    c->eci, cops_act_str(c->act), c->tac, c->earfcn, c->pci,
			    rsrp_dbm(c->rsrp));
	}
}

/* One group per PLMN with all its RATs and cells, sorted by PLMN. */
static void print_scan_groups(const struct shell *shell, const struct hio_lte_scan_result *r)
{
	size_t entries = MIN(r->count, ARRAY_SIZE(r->entries));
	struct scan_plmn plmns[ARRAY_SIZE(r->entries) + ARRAY_SIZE(r->cells)];
	size_t n = 0;
	size_t listed = 0;
	uint16_t mcc, mnc;

	for (size_t i = 0; i < entries; i++) {
		if (scan_entry_plmn(&r->entries[i], &mcc, &mnc)) {
			scan_plmn_add(plmns, &n, ARRAY_SIZE(plmns), mcc, mnc, r->entries[i].plmn);
		}
	}
	listed = n;

	for (size_t i = 0; i < r->cell_count; i++) {
		scan_plmn_add(plmns, &n, ARRAY_SIZE(plmns), r->cells[i].mcc, r->cells[i].mnc, NULL);
	}

	for (size_t i = 1; i < n; i++) {
		struct scan_plmn p = plmns[i];
		size_t j = i;

		for (; j > 0 && (plmns[j - 1].mcc > p.mcc ||
				 (plmns[j - 1].mcc == p.mcc && plmns[j - 1].mnc > p.mnc));
		     j--) {
			plmns[j] = plmns[j - 1];
		}
		plmns[j] = p;
	}

	for (size_t i = 0; i < n; i++) {
		print_scan_plmn(shell, r, entries, &plmns[i]);
	}

	if (entries < r->count) {
		shell_print(shell, "networks: %zu (%u with act, %zu stored), cells: %u", listed,
			    r->count, entries, r->cell_count);
	} else {
		shell_print(shell, "networks: %zu (%u with act), cells: %u", listed, r->count,
			    r->cell_count);
	}
}

static int print_scan_result(const struct shell *shell)
{
	struct hio_lte_scan_result result;

	int ret = hio_lte_get_scan_result(&result);
	if (ret) {
		shell_error(shell, "command failed");
		return ret;
	}

	if (!result.valid) {
		shell_print(shell, "no scan yet");
		return 0;
	}

	print_last_reject(shell);

	char ago[24];

	hio_util_fmt_duration((k_uptime_get() - result.uptime_ms) / MSEC_PER_SEC, ago, sizeof(ago));
	shell_print(shell, "last scan: %s ago (%s)", ago,
		    result.auto_triggered ? "auto" : "request");
	shell_print(shell, "search: lte-m cells %s, nb-iot cells %s, networks %s",
		    scan_status_str(result.cells_ltem_status),
		    scan_status_str(result.cells_nbiot_status), scan_status_str(result.plmn_status));

	print_scan_groups(shell, &result);

	return 0;
}

/* Test mode: no FSM, run the same steps directly. */
static void scan_test_mode(enum hio_lte_scan_mode mode)
{
	const k_timeout_t step_timeout = K_MINUTES(15);
	bool lte_m = strstr(g_hio_lte_config.mode, "lte-m");
	bool nb_iot = strstr(g_hio_lte_config.mode, "nb-iot");

	hio_lte_flow_scan_begin(mode, false);

	for (int i = 0; i < 2 && mode != HIO_LTE_SCAN_PLMN; i++) {
		bool nbiot_step = i == 1;

		if (nbiot_step ? !nb_iot : !lte_m) {
			continue;
		}

		hio_lte_flow_scan_rat(!nbiot_step, nbiot_step);
		k_sleep(K_SECONDS(2));

		uint8_t act = nbiot_step ? HIO_LTE_CEREG_PARAM_ACT_NBIOT : HIO_LTE_CEREG_PARAM_ACT_LTE;
		if (!hio_lte_flow_scan_cells_start(act) &&
		    hio_lte_flow_scan_cells_wait(step_timeout)) {
			hio_lte_flow_scan_abort();
			return;
		}
	}

	if (mode != HIO_LTE_SCAN_CELLS) {
		hio_lte_flow_scan_rat(lte_m, nb_iot);
		k_sleep(K_SECONDS(2));

		if (!hio_lte_flow_scan_plmn_start() && hio_lte_flow_scan_plmn_wait(step_timeout)) {
			hio_lte_flow_scan_abort();
			return;
		}
	}

	hio_lte_flow_scan_end();
}

static int cmd_scan(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	enum hio_lte_scan_mode mode = HIO_LTE_SCAN_ALL;

	if (argc > 2) {
		shell_error(shell, "only one argument is accepted");
		shell_help(shell);
		return -EINVAL;
	}

	if (argc == 2) {
		if (!strcmp(argv[1], "all")) {
			mode = HIO_LTE_SCAN_ALL;
		} else if (!strcmp(argv[1], "plmn")) {
			mode = HIO_LTE_SCAN_PLMN;
		} else if (!strcmp(argv[1], "cells")) {
			mode = HIO_LTE_SCAN_CELLS;
		} else {
			shell_error(shell, "unknown mode: %s", argv[1]);
			shell_help(shell);
			return -EINVAL;
		}
	}

	if (g_hio_lte_config.test) {
		shell_print(shell, "scanning (can take minutes)...");
		scan_test_mode(mode);
		ret = print_scan_result(shell);
		if (!ret) {
			shell_info(shell, "command succeeded");
		}
		return ret;
	}

	ret = hio_lte_scan(mode);
	if (ret == -EALREADY) {
		shell_print(shell, "scan already in progress, waiting for it");
	} else if (ret) {
		shell_error(shell, "command failed: %d", ret);
		return ret;
	}

	/* Cannot be interrupted anyway, so hold the shell until it is done. The
	 * connection is re-established after the scan. */
	shell_print(shell, "scanning, this takes minutes...");

	int64_t start = k_uptime_get();
	while (hio_lte_wait_for_scan(K_SECONDS(30))) {
		int64_t elapsed = (k_uptime_get() - start) / MSEC_PER_SEC;

		/* Beyond the scan watchdogs: something is wrong. */
		if (elapsed > 35 * 60) {
			shell_error(shell, "scan did not finish");
			return -ETIMEDOUT;
		}
		char took[24];

		hio_util_fmt_duration(elapsed, took, sizeof(took));
		shell_print(shell, "still scanning (%s)", took);
	}

	ret = print_scan_result(shell);
	if (!ret) {
		shell_info(shell, "command succeeded");
	}

	return ret;
}

static int cmd_scan_show(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	return print_scan_result(shell);
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

#if NCS_VERSION_NUMBER >= 0x30400
/* The shell bypass API gained a user_data parameter in NCS 3.4 (Zephyr 4.3) */
static void shell_bypass_cb(const struct shell *shell, uint8_t *data, size_t len,
			    void *user_data)
#else
static void shell_bypass_cb(const struct shell *shell, uint8_t *data, size_t len)
#endif
{
	static char line[256];
	static size_t line_len = 0;

	if (len == 0) {
		return;
	}

	if (strncmp((const char *)data, "+++", 3) == 0) {
		shell_print(shell, "exiting bypass mode");
		hio_lte_talk_bypass_set_cb(NULL, NULL);
#if NCS_VERSION_NUMBER >= 0x30400
		shell_set_bypass(shell, NULL, NULL);
#else
		shell_set_bypass(shell, NULL);
#endif
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
#if NCS_VERSION_NUMBER >= 0x30400
	shell_set_bypass(shell, shell_bypass_cb, NULL);
#else
	shell_set_bypass(shell, shell_bypass_cb);
#endif

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

static int cmd_ncellmeas_schedule(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (g_hio_lte_config.test) {
		shell_error(shell, "not supported in test mode");
		return -ENOEXEC;
	}

	ret = hio_lte_schedule_ncellmeas();
	if (ret && ret != -EALREADY) {
		LOG_ERR("Call `hio_lte_schedule_ncellmeas` failed: %d", ret);
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
	sub_lte_scan,

	SHELL_CMD_ARG(show, NULL,
	              "Show the last scan result.",
	              cmd_scan_show, 1, 0),

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

	SHELL_CMD_ARG(history, NULL,
	              "Show recent registration changes (CEREG, oldest first) and the "
	              "last reject.",
	              cmd_history, 1, 0),

	SHELL_CMD_ARG(scan, &sub_lte_scan,
	              "Scan cells and networks of the configured RATs and wait for the "
	              "result; interrupts attach and reconnects (format: "
	              "[all|plmn|cells]). Leaves the modem in CFUN=2 in test mode.",
	              cmd_scan, 1, 1),

	SHELL_CMD_ARG(ncellmeas-schedule, NULL,
	              "Schedule NCELLMEAS measurement.",
	              cmd_ncellmeas_schedule, 1, 0),


	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(lte, &sub_lte, "LTE commands.", print_help);
