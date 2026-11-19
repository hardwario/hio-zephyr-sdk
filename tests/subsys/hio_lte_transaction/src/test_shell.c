/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* 'lte scan' and 'lte ncellmeas' shell commands. */

#include "fsm_helpers.h"
#include "hio_lte_parse.h"
#include "hio_lte_state.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <ctype.h>
#include <errno.h>
#include <string.h>

extern atomic_t fake_flow_scan_plmn_count;
extern atomic_t fake_flow_scan_cells_count;
extern int64_t fake_flow_scan_rat_ms;
extern int64_t fake_flow_scan_cells_ms;
extern int64_t fake_flow_scan_plmn_ms;
extern atomic_t fake_flow_ncellmeas_count;
extern void fake_flow_ncellmeas_resp(const char *resp);

/* The fixed wait of test mode before a search. */
#define TEST_MODE_ATTACH_MS 20000

static const struct shell *m_sh;
static char m_buf[CONFIG_SHELL_BACKEND_DUMMY_BUF_SIZE + 1];
static const char *m_out;

/* The output without colours, CRs and leading empty lines. */
static void take_output(void)
{
	size_t size;
	const char *raw = shell_backend_dummy_get_output(m_sh, &size);
	size_t n = 0;

	for (const char *p = raw; *p && n < sizeof(m_buf) - 1; p++) {
		if (*p == '\x1b') {
			while (p[1] && !isalpha((unsigned char)p[1])) {
				p++;
			}
			if (p[1]) {
				p++;
			}
		} else if (*p != '\r') {
			m_buf[n++] = *p;
		}
	}
	m_buf[n] = '\0';

	for (m_out = m_buf; *m_out == '\n'; m_out++) {
	}
}

static int run(const char *cmd)
{
	shell_backend_dummy_clear_output(m_sh);
	int ret = shell_execute_cmd(m_sh, cmd);
	take_output();

	return ret;
}

#define assert_out(text)     zassert_not_null(strstr(m_out, text), "no '%s' in:\n%s", text, m_out)
#define assert_not_out(text) zassert_is_null(strstr(m_out, text), "'%s' in:\n%s", text, m_out)

/* A blocking command runs in its own thread, the test plays the modem. */
static K_THREAD_STACK_DEFINE(m_cmd_stack, 4096);
static struct k_thread m_cmd_thread;
static const char *m_cmd;
static int m_cmd_ret;

static void cmd_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	m_cmd_ret = shell_execute_cmd(m_sh, m_cmd);
}

static void cmd_start(const char *cmd)
{
	shell_backend_dummy_clear_output(m_sh);
	m_cmd = cmd;
	k_thread_create(&m_cmd_thread, m_cmd_stack, K_THREAD_STACK_SIZEOF(m_cmd_stack), cmd_entry,
			NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
}

static int cmd_join(void)
{
	zassert_ok(k_thread_join(&m_cmd_thread, K_MINUTES(20)), "%s did not return", m_cmd);
	take_output();

	return m_cmd_ret;
}

static void fsm_down(void)
{
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(10)));
}

static void *shell_setup(void)
{
	m_sh = shell_backend_dummy_get_ptr();
	WAIT_FOR(shell_ready(m_sh), 20000, k_msleep(1));

	return NULL;
}

static void shell_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* With CONFIG_HIO_LTE_NCELLMEAS, other suites leave the one of their
	 * enable. */
	fsm_end_ncellmeas();
	fsm_down();
	fsm_set_psm(0);
	strcpy(g_hio_lte_config.mode, "lte-m,nb-iot");
	atomic_clear(&fake_flow_scan_plmn_count);
	atomic_clear(&fake_flow_scan_cells_count);
	atomic_clear(&fake_flow_ncellmeas_count);
}

static void shell_after(void *fixture)
{
	ARG_UNUSED(fixture);

	fsm_end_ncellmeas();
	fsm_down();
}

ZTEST_SUITE(hio_lte_shell, NULL, shell_setup, shell_before, shell_after, NULL);

static void set_scan_result(void)
{
	struct hio_lte_scan_result result = {
		.valid = true,
		.mode = HIO_LTE_SCAN_PLMN,
		.uptime_ms = k_uptime_get(),
		.cells_ltem_status = -ENODATA,
		.cells_nbiot_status = -ENODATA,
		.count = 1,
		.entries = {{.stat = 2, .act = 7, .plmn = "23003"}},
	};

	hio_lte_state_set_scan_result(&result);
}

/* The modem registers and ends the PLMN step of a requested scan. */
static void finish_plmn_scan(void)
{
	zassert_true(fsm_in("scan"));
	fsm_scan_registers();
	for (int i = 0; i < 100 && atomic_get(&fake_flow_scan_plmn_count) == 0; i++) {
		k_sleep(K_MSEC(50));
	}
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1, "no PLMN search");

	set_scan_result();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
}

ZTEST(hio_lte_shell, test_scan_alone_prints_help)
{
	fsm_bring_up();

	zassert_ok(run("lte scan"));
	assert_out("now");
	assert_out("schedule");
	assert_out("show");
	assert_not_out("scanning");
	k_sleep(K_MSEC(50));
	zassert_false(fsm_in("scan"), "scan started");
}

ZTEST(hio_lte_shell, test_scan_unknown_word)
{
	zassert_equal(run("lte scan foo"), -EINVAL);
	assert_out("command not found: foo");
	assert_out("schedule");
}

ZTEST(hio_lte_shell, test_scan_now_bad_mode)
{
	zassert_equal(run("lte scan now foo"), -EINVAL);
	assert_out("unknown mode: foo");
}

ZTEST(hio_lte_shell, test_scan_now_waits_for_result)
{
	fsm_bring_up();

	cmd_start("lte scan now plmn");
	k_sleep(K_SECONDS(31));
	finish_plmn_scan();

	zassert_ok(cmd_join());
	assert_out("scanning, this takes minutes...");
	assert_out("still scanning (30s)");
	assert_out("plmn: 23003, act: lte-m, stat: current");
	assert_out("command succeeded");
	assert_not_out("already");
}

ZTEST(hio_lte_shell, test_scan_now_already_in_progress)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));

	cmd_start("lte scan now");
	k_sleep(K_MSEC(50));
	finish_plmn_scan();

	zassert_ok(cmd_join());
	assert_out("scan already in progress, waiting for it");
	assert_out("scanning, this takes minutes...");
	assert_out("command succeeded");
}

ZTEST(hio_lte_shell, test_scan_now_in_test_mode)
{
	g_hio_lte_config.test = true;

	zassert_ok(run("lte scan now plmn"));
	assert_out("scanning (can take minutes)...");
	assert_out("command succeeded");
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);
}

/* No CSCON or CEREG events in test mode: a fixed wait after the switch to
 * CFUN=1 lets the attach register and release its RRC connection. */
ZTEST(hio_lte_shell, test_scan_now_in_test_mode_waits_for_attach)
{
	g_hio_lte_config.test = true;
	strcpy(g_hio_lte_config.mode, "nb-iot");

	zassert_ok(run("lte scan now cells"));
	assert_out("command succeeded");
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);
	zassert_true(fake_flow_scan_cells_ms - fake_flow_scan_rat_ms >= TEST_MODE_ATTACH_MS,
		     "cells searched %lld ms after the switch",
		     fake_flow_scan_cells_ms - fake_flow_scan_rat_ms);

	zassert_ok(run("lte scan now plmn"));
	assert_out("command succeeded");
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);
	zassert_true(fake_flow_scan_plmn_ms - fake_flow_scan_rat_ms >= TEST_MODE_ATTACH_MS,
		     "networks searched %lld ms after the switch",
		     fake_flow_scan_plmn_ms - fake_flow_scan_rat_ms);
}

ZTEST(hio_lte_shell, test_scan_schedule)
{
	fsm_bring_up();

	zassert_ok(run("lte scan schedule plmn"));
	assert_out("command succeeded");
	assert_not_out("scanning");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_equal(hio_lte_wait_for_scan(K_NO_WAIT), -ETIMEDOUT);

	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1, "not the PLMN mode");
	zassert_ok(run("lte scan schedule"), "already scanning is not an error");
	assert_out("command succeeded");
}

ZTEST(hio_lte_shell, test_scan_schedule_bad_mode)
{
	zassert_equal(run("lte scan schedule foo"), -EINVAL);
	assert_out("unknown mode: foo");
}

ZTEST(hio_lte_shell, test_scan_schedule_in_test_mode)
{
	g_hio_lte_config.test = true;

	zassert_equal(run("lte scan schedule"), -ENOEXEC);
	assert_out("not supported in test mode");
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 0);
}

/* A network only the cell search found, with a three-digit MNC. */
ZTEST(hio_lte_shell, test_scan_show_three_digit_mnc)
{
	struct hio_lte_scan_result result = {
		.valid = true,
		.mode = HIO_LTE_SCAN_CELLS,
		.uptime_ms = k_uptime_get(),
		.cells_ltem_status = 0,
		.cells_nbiot_status = -ENODATA,
		.plmn_status = -ENODATA,
		.cell_count = 1,
		.cells = {{.eci = 0x11B07,
			   .mcc = 310,
			   .mnc = 4,
			   .mnc_digits = 3,
			   .rsrp = 63,
			   .act = HIO_LTE_CEREG_PARAM_ACT_LTE}},
	};

	hio_lte_state_set_scan_result(&result);

	zassert_ok(run("lte scan show"));
	assert_out("plmn: 310004, act: lte-m, stat: not in network list\n  cell: 00011B07");
}

ZTEST(hio_lte_shell, test_scan_schedule_fails_when_disabled)
{
	zassert_equal(run("lte scan schedule"), -ENODEV);
	assert_out("command failed");
	assert_not_out("command succeeded");
}

/* Two cells, each with its own neighbours. */
static void set_ncellmeas(void)
{
	struct hio_lte_ncellmeas_param param;

	zassert_ok(hio_lte_parse_urc_ncellmeas(
		"0,\"00011B07\",\"26295\",\"00B7\",10512,9034,2300,7,63,31,150344527,1,2,"
		"2300,8,60,29,92,2400,10,56,27,162,"
		"\"00011B08\",\"23003\",\"8DCC\",65535,0,3544,9,62,30,150345527,0,1,"
		"6300,11,40,20,10",
		5, &param));
	hio_lte_state_set_ncellmeas_param(&param);
}

static void assert_ncellmeas_cells(void)
{
	assert_out("cell: 00011B07, plmn: 26295, tac: 00B7, earfcn: 2300, pci: 7, rsrp: -78 dBm, "
		   "serving\n"
		   "  neighbour: earfcn: 2300, pci: 8, rsrp: -81 dBm\n"
		   "  neighbour: earfcn: 2400, pci: 10, rsrp: -85 dBm\n"
		   "cell: 00011B08, plmn: 23003, tac: 8DCC, earfcn: 3544, pci: 9, rsrp: -79 dBm\n"
		   "  neighbour: earfcn: 6300, pci: 11, rsrp: -101 dBm\n"
		   "command succeeded");
}

ZTEST(hio_lte_shell, test_ncellmeas_alone_prints_help)
{
	zassert_ok(run("lte ncellmeas"));
	assert_out("now");
	assert_out("schedule");
	assert_out("show");
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "requested a measurement");
}

ZTEST(hio_lte_shell, test_ncellmeas_unknown_word)
{
	zassert_equal(run("lte ncellmeas foo"), -EINVAL);
	assert_out("command not found: foo");
	assert_out("schedule");
}

ZTEST(hio_lte_shell, test_ncellmeas_schedule_command_removed)
{
	zassert_equal(run("lte ncellmeas-schedule"), -EINVAL);
	assert_out("command not found: ncellmeas-schedule");
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
}

ZTEST(hio_lte_shell, test_ncellmeas_show_none)
{
	hio_lte_state_set_ncellmeas_param(&(struct hio_lte_ncellmeas_param){0});

	zassert_ok(run("lte ncellmeas show"));
	zassert_str_equal(m_out, "no measurement yet\ncommand succeeded\n");
}

ZTEST(hio_lte_shell, test_ncellmeas_show)
{
	set_ncellmeas();
	k_sleep(K_SECONDS(5));

	zassert_ok(run("lte ncellmeas show"));
	assert_out("ncellmeas: 5s ago, ok\ncell: 00011B07");
	assert_ncellmeas_cells();
}

ZTEST(hio_lte_shell, test_ncellmeas_show_failed)
{
	struct hio_lte_ncellmeas_param param;

	zassert_ok(hio_lte_parse_urc_ncellmeas("1", 5, &param));
	hio_lte_state_set_ncellmeas_param(&param);

	zassert_ok(run("lte ncellmeas show"));
	zassert_str_equal(m_out, "ncellmeas: 0s ago, failed\ncommand succeeded\n");
}

ZTEST(hio_lte_shell, test_ncellmeas_show_other_status)
{
	struct hio_lte_ncellmeas_param param;

	zassert_ok(hio_lte_parse_urc_ncellmeas(
		"3,\"00011B07\",\"26295\",\"00B7\",10512,9034,2300,7,63,31,150344527,1,0", 5,
		&param));
	hio_lte_state_set_ncellmeas_param(&param);

	zassert_ok(run("lte ncellmeas show"));
	assert_out("ncellmeas: 0s ago, status 3\ncell: 00011B07");
}

/* More neighbours announced than stored: only the stored ones. */
ZTEST(hio_lte_shell, test_ncellmeas_show_stored_neighbours_only)
{
	struct hio_lte_ncellmeas_param param = {
		.valid = true,
		.num_cells = 1,
		.num_ncells = 1,
		.cells = {{.eci = 0x11B07, .mcc = 262, .mnc = 95, .neighbor_count = 3}},
		.ncells = {{.earfcn = 6300, .pci = 11, .rsrp = 40}},
	};

	hio_lte_state_set_ncellmeas_param(&param);

	zassert_ok(run("lte ncellmeas show"));
	assert_out("  neighbour: earfcn: 6300, pci: 11, rsrp: -101 dBm\ncommand succeeded");
}

ZTEST(hio_lte_shell, test_ncellmeas_show_in_progress)
{
	set_ncellmeas();
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);

	zassert_ok(run("lte ncellmeas show"));
	zassert_str_equal(m_out, "ncellmeas: in progress\ncommand succeeded\n");
}

ZTEST(hio_lte_shell, test_ncellmeas_now)
{
	hio_lte_state_set_ncellmeas_param(&(struct hio_lte_ncellmeas_param){0});
	fsm_bring_up();

	cmd_start("lte ncellmeas now");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	k_sleep(K_SECONDS(31));

	set_ncellmeas();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);

	zassert_ok(cmd_join());
	assert_out("measuring, this takes minutes...\n"
		   "still measuring (30s)\n"
		   "ncellmeas: 0s ago, ok\n");
	assert_ncellmeas_cells();
	if (IS_ENABLED(CONFIG_HIO_LTE_NCELLMEAS)) {
		/* The one of the enable, measured at once now. */
		assert_out("measurement already in progress, waiting for it\n");
	} else {
		assert_not_out("already");
	}
}

ZTEST(hio_lte_shell, test_ncellmeas_now_already_in_progress)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);

	cmd_start("lte ncellmeas now");
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));

	set_ncellmeas();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);

	zassert_ok(cmd_join());
	assert_out("measurement already in progress, waiting for it\n"
		   "measuring, this takes minutes...\n"
		   "ncellmeas: 0s ago, ok\n");
	assert_ncellmeas_cells();
}

/* Not measured in time (here the connection is never released): reported
 * after 10 min, not waited for forever. */
ZTEST(hio_lte_shell, test_ncellmeas_now_gives_up)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));

	cmd_start("lte ncellmeas now");

	zassert_equal(cmd_join(), -ETIMEDOUT);
	assert_out("still measuring (9m 30s)\nmeasurement did not finish");
	assert_not_out("10m");
	assert_not_out("command succeeded");
}

ZTEST(hio_lte_shell, test_ncellmeas_now_fails_when_disabled)
{
	zassert_equal(run("lte ncellmeas now"), -ENODEV);
	assert_out("command failed: -19");
	assert_not_out("measuring");
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "requested");
}

/* The modem refuses the command: "failed" at once. */
ZTEST(hio_lte_shell, test_ncellmeas_now_refused)
{
	fsm_bring_up();

	cmd_start("lte ncellmeas now");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	fake_flow_ncellmeas_resp("+CME ERROR: 514\r\n");

	zassert_ok(cmd_join());
	assert_out("ncellmeas: 0s ago, failed\ncommand succeeded");
	assert_not_out("still measuring");
}

/* The PLMN as the modem wrote it, with a three-digit MNC. */
ZTEST(hio_lte_shell, test_ncellmeas_show_three_digit_mnc)
{
	struct hio_lte_ncellmeas_param param;

	zassert_ok(hio_lte_parse_urc_ncellmeas(
		"0,\"00011B07\",\"310004\",\"00B7\",10512,9034,2300,7,63,31,150344527,1,0", 5,
		&param));
	hio_lte_state_set_ncellmeas_param(&param);

	zassert_ok(run("lte ncellmeas show"));
	assert_out("cell: 00011B07, plmn: 310004, tac: 00B7");
}

ZTEST(hio_lte_shell, test_ncellmeas_now_in_test_mode)
{
	g_hio_lte_config.test = true;

	zassert_equal(run("lte ncellmeas now"), -ENOEXEC);
	assert_out("not supported in test mode");
	assert_not_out("measuring");
}

ZTEST(hio_lte_shell, test_ncellmeas_schedule)
{
	fsm_bring_up();

	zassert_ok(run("lte ncellmeas schedule"));
	zassert_str_equal(m_out, "command succeeded\n");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));

	zassert_ok(run("lte ncellmeas schedule"), "already measuring is not an error");
	zassert_str_equal(m_out, "command succeeded\n");
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
}

ZTEST(hio_lte_shell, test_ncellmeas_schedule_in_test_mode)
{
	g_hio_lte_config.test = true;

	zassert_equal(run("lte ncellmeas schedule"), -ENOEXEC);
	assert_out("not supported in test mode");
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
}
