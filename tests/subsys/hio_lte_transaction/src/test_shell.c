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
extern char fake_flow_scan_trace[];
extern int fake_flow_scan_wait_ret;
extern atomic_t fake_flow_scan_abort_count;
extern void (*fake_flow_scan_rat_hook)(void);
extern atomic_t fake_flow_ncellmeas_test_count;
extern void fake_flow_ncellmeas_done(const char *error);
extern uint32_t fake_flow_stop_ms;
extern uint32_t fake_attach_timeout_ms;
extern uint32_t fake_retry_delay_ms;

static const struct hio_lte_socket_config m_cfg = {.port = 5002, .addr = "192.0.2.1"};

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
	zassert_ok(k_thread_join(&m_cmd_thread, K_MINUTES(40)), "%s did not return", m_cmd);
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
	atomic_clear(&fake_flow_ncellmeas_test_count);
	atomic_clear(&fake_flow_scan_abort_count);
	fake_flow_scan_trace[0] = '\0';
	fake_flow_scan_wait_ret = 0;
	fake_flow_scan_rat_hook = NULL;
	fake_flow_stop_ms = 0;
}

static struct hio_lte_cb m_slow_scan_done_cb;

static void shell_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)hio_lte_remove_callback(&m_slow_scan_done_cb);
	fake_flow_stop_ms = 0;
	fake_attach_timeout_ms = 60000;
	fake_retry_delay_ms = 0;
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

/* The modem registers and ends the network search of each RAT of a requested
 * PLMN scan. */
static void finish_plmn_scan(void)
{
	for (int rat = 1; rat <= 2; rat++) {
		zassert_true(fsm_in("scan"));
		fsm_scan_registers();
		for (int i = 0; i < 100 && atomic_get(&fake_flow_scan_plmn_count) < rat; i++) {
			k_sleep(K_MSEC(50));
		}
		zassert_equal(atomic_get(&fake_flow_scan_plmn_count), rat, "no PLMN search");

		set_scan_result();
		fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
		k_sleep(K_MSEC(50));
	}
}

/* With the flow check passing, ERROR resumes in 5 s. */
static void fsm_to_error(void)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_ERROR);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("error"));
}

/* PREPARE ignores a request, ATTACH takes it. */
static void fsm_to_prepare(void)
{
	zassert_ok(hio_lte_enable(&m_cfg));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
}

/* Deregistered, the third attach fails: its retry delay, below the one of the
 * automatic scan. */
static void fsm_to_retry_delay(void)
{
	int attempt = 0;

	fsm_bring_up();
	fsm_boot_ncellmeas();
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));
	fake_attach_timeout_ms = 100;
	fake_retry_delay_ms = 5 * 60 * 1000;
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_DEREGISTERED);

	/* Two zero delays (5 s CFUN=4 settle each) before the third attempt. */
	for (int i = 0; i < 300; i++) {
		zassert_ok(hio_lte_get_curr_attach_info(&attempt, NULL, NULL, NULL));
		if (attempt == 3 && fsm_in("retry_delay")) {
			break;
		}
		if (fsm_in("prepare")) {
			fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
		}
		k_sleep(K_MSEC(100));
	}

	zassert_equal(attempt, 3);
	zassert_true(fsm_in("retry_delay"));
	/* Past its CFUN=4 settle, in the delay itself. */
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("retry_delay"));
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

/* Test mode: no FSM, the steps run directly, per RAT: the switch, the fixed
 * wait for the attach, the cell search, the network search. */
ZTEST(hio_lte_shell, test_scan_now_in_test_mode)
{
	g_hio_lte_config.test = true;
	set_scan_result();

	zassert_ok(run("lte scan now"));
	/* Back in both configured RATs at the end. */
	zassert_str_equal(fake_flow_scan_trace,
			  "rat:m cells:7 plmn:7 rat:n cells:9 plmn:9 rat:mn ");
	assert_out("scanning, this takes minutes...\nstill scanning (30s)\n");
	assert_out("plmn: 23003, act: lte-m, stat: current");
	assert_out("cells: 0\ncommand succeeded\n");
	assert_not_out("still scanning (1m");
	assert_not_out("not possible");
}

ZTEST(hio_lte_shell, test_scan_now_in_test_mode_modes)
{
	g_hio_lte_config.test = true;

	zassert_ok(run("lte scan now plmn"));
	zassert_str_equal(fake_flow_scan_trace, "rat:m plmn:7 rat:n plmn:9 rat:mn ");

	fake_flow_scan_trace[0] = '\0';
	zassert_ok(run("lte scan now cells"));
	zassert_str_equal(fake_flow_scan_trace, "rat:m cells:7 rat:n cells:9 rat:mn ");
}

/* No CSCON or CEREG events in test mode: a fixed wait after the switch to
 * CFUN=1 lets the attach register and release its RRC connection. */
ZTEST(hio_lte_shell, test_scan_now_in_test_mode_waits_for_attach)
{
	g_hio_lte_config.test = true;
	strcpy(g_hio_lte_config.mode, "nb-iot");

	zassert_ok(run("lte scan now"));
	assert_out("command succeeded");
	zassert_str_equal(fake_flow_scan_trace, "rat:n cells:9 plmn:9 ");
	zassert_true(fake_flow_scan_cells_ms - fake_flow_scan_rat_ms >= TEST_MODE_ATTACH_MS,
		     "cells searched %lld ms after the switch",
		     fake_flow_scan_cells_ms - fake_flow_scan_rat_ms);
	zassert_true(fake_flow_scan_plmn_ms - fake_flow_scan_cells_ms < 100,
		     "networks not searched after the cells");

	zassert_ok(run("lte scan now plmn"));
	assert_out("command succeeded");
	zassert_true(fake_flow_scan_plmn_ms - fake_flow_scan_rat_ms >= TEST_MODE_ATTACH_MS,
		     "networks searched %lld ms after the switch",
		     fake_flow_scan_plmn_ms - fake_flow_scan_rat_ms);
}

/* A search that does not end: the scan gives up 35 min after its start. */
ZTEST(hio_lte_shell, test_scan_now_in_test_mode_gives_up)
{
	g_hio_lte_config.test = true;
	fake_flow_scan_wait_ret = -EAGAIN;

	zassert_equal(run("lte scan now cells"), -ETIMEDOUT);
	assert_out("still scanning (35m 00s)\nscan did not finish\n");
	assert_not_out("35m 30s");
	assert_not_out("command succeeded");
	zassert_str_equal(fake_flow_scan_trace, "rat:m cells:7 ");
	zassert_equal(atomic_get(&fake_flow_scan_abort_count), 1, "search not stopped");
}

ZTEST(hio_lte_shell, test_scan_now_refused_when_disabled)
{
	zassert_equal(run("lte scan now"), -EBUSY);
	zassert_str_equal(m_out, "scan not possible: LTE is disabled (fsm-state: disabled)\n");
}

ZTEST(hio_lte_shell, test_scan_now_refused_in_error)
{
	fsm_to_error();

	zassert_equal(run("lte scan now"), -EBUSY);
	zassert_str_equal(m_out, "scan not possible: LTE error, retrying (fsm-state: error)\n");
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("ready"), "a scan was requested");
}

ZTEST(hio_lte_shell, test_scan_now_refused_in_reset_loop)
{
	fsm_to_prepare();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_RESET_LOOP);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("reset_loop"));

	zassert_equal(run("lte scan now"), -EBUSY);
	zassert_str_equal(m_out, "scan not possible: modem reset loop, attach blocked "
				 "(fsm-state: reset_loop)\n");
}

/* An error before the scan starts ends the wait; the request stays. */
ZTEST(hio_lte_shell, test_scan_now_blocked_while_waiting)
{
	fsm_to_prepare();

	cmd_start("lte scan now");
	k_sleep(K_SECONDS(2));
	zassert_true(fsm_in("prepare"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_ERROR);

	zassert_equal(cmd_join(), -EBUSY);
	zassert_str_equal(m_out, "scanning, this takes minutes...\n"
				 "scan not possible: LTE error, retrying (fsm-state: error)\n");
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("scan"), "the request did not stay");
}

/* Dropped before it started: no result of its own. The modem takes its
 * time to stop, the state reads disabled before the request is dropped. */
ZTEST(hio_lte_shell, test_scan_now_dropped)
{
	set_scan_result();
	fsm_to_prepare();

	cmd_start("lte scan now");
	k_sleep(K_SECONDS(2));
	fake_flow_stop_ms = 1500;
	zassert_ok(hio_lte_disable());

	zassert_equal(cmd_join(), -ECANCELED);
	zassert_str_equal(m_out, "scanning, this takes minutes...\nscan dropped\n");
}

/* A reconnect drops it the same way, LTE comes back at once. */
ZTEST(hio_lte_shell, test_scan_now_dropped_by_reconnect)
{
	set_scan_result();
	fsm_to_prepare();

	cmd_start("lte scan now");
	k_sleep(K_SECONDS(2));
	fake_flow_stop_ms = 1500;
	zassert_ok(hio_lte_reconnect());

	zassert_equal(cmd_join(), -ECANCELED);
	zassert_str_equal(m_out, "scanning, this takes minutes...\nscan dropped\n");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"), "LTE not back");
}

/* Ended while running: what it has is its result. */
ZTEST(hio_lte_shell, test_scan_now_cut_short)
{
	set_scan_result();
	fsm_bring_up();

	cmd_start("lte scan now plmn");
	k_sleep(K_SECONDS(2));
	zassert_true(fsm_in("scan"));
	zassert_ok(hio_lte_disable());

	zassert_ok(cmd_join());
	assert_out("plmn: 23003, act: lte-m, stat: current");
	assert_out("command succeeded");
	assert_not_out("dropped");
}

/* The lines count from the start of the scan, not from the command. */
ZTEST(hio_lte_shell, test_scan_now_counts_from_scan_start)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_SECONDS(20));

	cmd_start("lte scan now");
	k_sleep(K_SECONDS(15));
	finish_plmn_scan();

	zassert_ok(cmd_join());
	assert_out("scan already in progress, waiting for it\n"
		   "scanning, this takes minutes...\n"
		   "still scanning (30s)\n");
	assert_out("command succeeded");
}

static void slow_scan_done(struct hio_lte_cb *cb, enum hio_lte_event event)
{
	ARG_UNUSED(cb);

	if (event == HIO_LTE_EVENT_SCAN_DONE) {
		k_sleep(K_MSEC(200));
	}
}

/* Requested as a scan ends, before the FSM leaves it: that scan is not this
 * request's, the next one is. */
ZTEST(hio_lte_shell, test_scan_now_as_scan_ends)
{
	fsm_bring_up();
	m_slow_scan_done_cb.handler = slow_scan_done;
	zassert_ok(hio_lte_add_callback(&m_slow_scan_done_cb));
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	finish_plmn_scan();
	zassert_true(fsm_in("scan"), "the callback does not hold the FSM");

	cmd_start("lte scan now plmn");
	k_sleep(K_MSEC(300));
	zassert_true(fsm_in("prepare"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	atomic_clear(&fake_flow_scan_plmn_count);
	finish_plmn_scan();

	zassert_ok(cmd_join());
	assert_not_out("already");
	assert_not_out("dropped");
	assert_out("plmn: 23003, act: lte-m, stat: current");
	assert_out("command succeeded");
}

static bool m_blocked;

/* The first RAT switch holds the FSM for 36 min. */
static void block_scan(void)
{
	if (!m_blocked) {
		m_blocked = true;
		k_sleep(K_MINUTES(36));
	}
}

/* Bounded 35 min from the start of the scan. */
ZTEST(hio_lte_shell, test_scan_now_gives_up)
{
	fsm_bring_up();
	m_blocked = false;
	fake_flow_scan_rat_hook = block_scan;
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_SECONDS(20));

	cmd_start("lte scan now");

	zassert_equal(cmd_join(), -ETIMEDOUT);
	assert_out("still scanning (35m 00s)\nscan did not finish\n");
	assert_not_out("35m 30s");
	assert_not_out("command succeeded");
	k_sleep(K_MINUTES(1));
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
	assert_out("networks: 0 (0 with act), cells: 1\ncommand succeeded\n");
}

ZTEST(hio_lte_shell, test_scan_show_none)
{
	hio_lte_state_set_scan_result(&(struct hio_lte_scan_result){0});

	zassert_ok(run("lte scan show"));
	zassert_str_equal(m_out, "no scan yet\ncommand succeeded\n");
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

ZTEST(hio_lte_shell, test_ncellmeas_now_refused_when_disabled)
{
	zassert_equal(run("lte ncellmeas now"), -EBUSY);
	zassert_str_equal(m_out,
			  "measurement not possible: LTE is disabled (fsm-state: disabled)\n");
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "requested");
}

ZTEST(hio_lte_shell, test_ncellmeas_now_refused_in_error)
{
	fsm_to_error();

	zassert_equal(run("lte ncellmeas now"), -EBUSY);
	zassert_str_equal(m_out,
			  "measurement not possible: LTE error, retrying (fsm-state: error)\n");
	if (!IS_ENABLED(CONFIG_HIO_LTE_NCELLMEAS)) {
		zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "requested");
	}
}

/* The retry delay takes a scan, a measurement would wait there for the next
 * attach (up to hours): none is requested. */
ZTEST(hio_lte_shell, test_ncellmeas_now_refused_in_retry_delay)
{
	fsm_to_retry_delay();

	zassert_equal(run("lte ncellmeas now"), -EBUSY);
	zassert_str_equal(m_out, "measurement not possible: not registered, retrying attach "
				 "(fsm-state: retry_delay)\n");
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "requested");
	zassert_true(fsm_in("retry_delay"));
}

ZTEST(hio_lte_shell, test_ncellmeas_now_refused_while_scanning)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));

	zassert_equal(run("lte ncellmeas now"), -EBUSY);
	zassert_str_equal(m_out, "measurement not possible: scan in progress (fsm-state: scan)\n");
	if (!IS_ENABLED(CONFIG_HIO_LTE_NCELLMEAS)) {
		zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "requested");
	}
}

/* A requested scan, not started yet, counts as well. */
ZTEST(hio_lte_shell, test_ncellmeas_now_refused_while_scan_requested)
{
	fsm_to_prepare();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));

	zassert_equal(run("lte ncellmeas now"), -EBUSY);
	zassert_str_equal(m_out,
			  "measurement not possible: scan in progress (fsm-state: prepare)\n");
}

ZTEST(hio_lte_shell, test_ncellmeas_schedule_refused_while_scanning)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));

	zassert_equal(run("lte ncellmeas schedule"), -EBUSY);
	zassert_str_equal(m_out, "scan in progress\n");
}

/* A disable drops the request. */
ZTEST(hio_lte_shell, test_ncellmeas_now_dropped)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));

	cmd_start("lte ncellmeas now");
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));
	zassert_ok(hio_lte_disable());

	zassert_equal(cmd_join(), -ECANCELED);
	assert_out("measuring, this takes minutes...\nmeasurement dropped\n");
	assert_not_out("command succeeded");
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

/* Test mode: no FSM, the measurement runs directly and is stored as the
 * FSM's. */
ZTEST(hio_lte_shell, test_ncellmeas_now_in_test_mode)
{
	g_hio_lte_config.test = true;
	hio_lte_state_set_ncellmeas_param(&(struct hio_lte_ncellmeas_param){0});

	cmd_start("lte ncellmeas now");
	k_sleep(K_SECONDS(31));
	set_ncellmeas();
	fake_flow_ncellmeas_done(NULL);

	zassert_ok(cmd_join());
	assert_out("measuring, this takes minutes...\n"
		   "still measuring (30s)\n"
		   "ncellmeas: 0s ago, ok\n");
	assert_ncellmeas_cells();
	zassert_equal(atomic_get(&fake_flow_ncellmeas_test_count), 1);
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0, "measured by the FSM");

	zassert_ok(run("lte ncellmeas show"));
	assert_ncellmeas_cells();
}

ZTEST(hio_lte_shell, test_ncellmeas_now_in_test_mode_refused)
{
	g_hio_lte_config.test = true;

	cmd_start("lte ncellmeas now");
	k_sleep(K_MSEC(50));
	fake_flow_ncellmeas_done("+CME ERROR: 514");

	zassert_equal(cmd_join(), -EIO);
	zassert_str_equal(m_out, "measuring, this takes minutes...\n"
				 "measurement not possible: +CME ERROR: 514\n");
}

/* A URC that does not parse stores nothing: not the measurement of before. */
ZTEST(hio_lte_shell, test_ncellmeas_now_in_test_mode_unreadable)
{
	g_hio_lte_config.test = true;
	set_ncellmeas();

	cmd_start("lte ncellmeas now");
	k_sleep(K_MSEC(50));
	fake_flow_ncellmeas_done(NULL);

	zassert_equal(cmd_join(), -EBADMSG);
	zassert_str_equal(m_out, "measuring, this takes minutes...\n"
				 "measurement not possible: unreadable result\n");
}

ZTEST(hio_lte_shell, test_ncellmeas_now_in_test_mode_gives_up)
{
	g_hio_lte_config.test = true;

	cmd_start("lte ncellmeas now");

	zassert_equal(cmd_join(), -ETIMEDOUT);
	assert_out("still measuring (9m 30s)\nmeasurement did not finish\n");
	assert_not_out("10m");
	assert_not_out("command succeeded");
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
