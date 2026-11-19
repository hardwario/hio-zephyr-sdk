/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Network without PSM: the ready state switches the modem off (CFUN=4) and the
 * modem reports "+CEREG: 0", which must not hide the last registration. */

#include "fsm_helpers.h"
#include "hio_lte_parse.h"
#include "hio_lte_state.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

extern int fake_flow_last_cfun;
extern void (*fake_flow_cfun_hook)(int cfun);
extern atomic_t fake_flow_scan_plmn_count;

static void set_cereg(enum hio_lte_cereg_param_stat stat)
{
	struct hio_lte_cereg_param param = {
		.valid = true,
		.stat = stat,
		.cid = 0xAE520,
		.act = HIO_LTE_CEREG_PARAM_ACT_NBIOT,
		.active_time = -1,
		.periodic_tau_ext = -1,
	};

	if (stat == HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING) {
		strcpy(param.tac, "B4DC");
	} else {
		/* As parsed from "+CEREG: <stat>". */
		param.cid = 0;
		param.act = 0;
		param.active_time = GRPS_TIMER_INVALID;
		param.periodic_tau_ext = GRPS_TIMER_INVALID;
	}

	hio_lte_state_set_cereg_param(&param);
}

static const char *shell_cmd(const char *cmd)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t size;

	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	shell_backend_dummy_clear_output(sh);
	int ret = shell_execute_cmd(sh, cmd);
	const char *out = shell_backend_dummy_get_output(sh, &size);
	zassert_ok(ret, "%s failed: %d: %s", cmd, ret, out);

	return out;
}

static void cfun4_reports_not_registered(int cfun)
{
	if (cfun == 4) {
		set_cereg(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
	}
}

static void fsm_down(void)
{
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(2)));
}

static void no_psm_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fsm_down();
	/* The scan has a step per configured RAT. */
	strcpy(g_hio_lte_config.mode, "lte-m,nb-iot");
	fake_flow_last_cfun = -1;
	fake_flow_cfun_hook = cfun4_reports_not_registered;
}

static void no_psm_after(void *fixture)
{
	ARG_UNUSED(fixture);

	fsm_down();
	fake_flow_cfun_hook = NULL;
}

ZTEST_SUITE(hio_lte_no_psm, NULL, NULL, no_psm_before, no_psm_after, NULL);

/* Ready, PSM not granted: wait for the ready timeout to switch the radio off. */
static void radio_off_after_ready(void)
{
	set_cereg(HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING);
	fsm_bring_up();
	/* The radio stays on for a requested measurement. */
	fsm_boot_ncellmeas();

	for (int i = 0; i < 40 && fake_flow_last_cfun != 4; i++) {
		k_sleep(K_MSEC(50));
	}

	zassert_equal(fake_flow_last_cfun, 4, "no CFUN=4 without PSM");
}

static void assert_cereg_stat(enum hio_lte_cereg_param_stat stat)
{
	struct hio_lte_cereg_param out;

	zassert_ok(hio_lte_get_cereg_param(&out));
	zassert_equal(out.stat, stat, "stat %d", out.stat);
}

ZTEST(hio_lte_no_psm, test_cfun4_keeps_registration_until_attach)
{
	radio_off_after_ready();

	struct hio_lte_cereg_param out;

	zassert_ok(hio_lte_get_cereg_param(&out));
	zassert_equal(out.stat, HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING);
	zassert_str_equal(out.tac, "B4DC");
	zassert_equal(out.act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);
	zassert_equal(out.active_time, -1);

	const char *text = shell_cmd("lte state");
	zassert_not_null(strstr(text, "cereg: registered-roaming"), "%s", text);
	zassert_not_null(strstr(text, "psm: not granted"), "%s", text);

	/* Wake: CFUN=1 and attach, where a loss of the network counts again. */
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_XMODEMSLEEP);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("sleep"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_READY);
	k_sleep(K_MSEC(50));
	zassert_equal(fake_flow_last_cfun, 1);
	zassert_true(fsm_in("attach"));

	set_cereg(HIO_LTE_CEREG_PARAM_STAT_SEARCHING);
	assert_cereg_stat(HIO_LTE_CEREG_PARAM_STAT_SEARCHING);

	/* No PSM line without a registration. */
	text = shell_cmd("lte state");
	zassert_not_null(strstr(text, "cereg: searching"), "%s", text);
	zassert_is_null(strstr(text, "psm:"), "%s", text);
}

ZTEST(hio_lte_no_psm, test_state_shows_granted_psm)
{
	struct hio_lte_cereg_param param = {
		.valid = true,
		.stat = HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME,
		.act = HIO_LTE_CEREG_PARAM_ACT_LTE,
		.active_time = 0,
		.periodic_tau_ext = 86400,
	};

	hio_lte_state_set_cereg_param(&param);

	const char *text = shell_cmd("lte state");
	zassert_not_null(strstr(text, "psm: active-time 0s, periodic-tau 1d 00h 00m 00s"), "%s",
			 text);
}

ZTEST(hio_lte_no_psm, test_disable_turns_radio_on)
{
	radio_off_after_ready();

	fsm_down();

	set_cereg(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
	assert_cereg_stat(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
}

ZTEST(hio_lte_no_psm, test_error_turns_radio_on)
{
	radio_off_after_ready();

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_ERROR);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("error"));

	set_cereg(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
	assert_cereg_stat(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
}

ZTEST(hio_lte_no_psm, test_scan_turns_radio_on)
{
	radio_off_after_ready();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));

	set_cereg(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
	assert_cereg_stat(HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED);
}

/* The modem is in CFUN=4 already, the first switch of the scan brings no
 * "+CEREG: 0": the NB-IoT registration of before must not count for the
 * network search in LTE-M. */
ZTEST(hio_lte_no_psm, test_scan_from_radio_off_waits_for_registration)
{
	radio_off_after_ready();
	atomic_clear(&fake_flow_scan_plmn_count);

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_ALL));
	/* No CSCON: the cell search starts after the attach window. */
	k_sleep(K_MSEC(10200));
	zassert_true(fsm_in("scan"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 0,
		      "searched on the registration of before");

	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1, "not searched once registered");
}
