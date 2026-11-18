/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Network without PSM: the ready state switches the modem off (CFUN=4) and the
 * modem reports "+CEREG: 0", which must not hide the last registration. */

#include "fsm_helpers.h"
#include "hio_lte_state.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern int fake_flow_last_cfun;
extern void (*fake_flow_cfun_hook)(int cfun);

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
		param.active_time = 0;
		param.periodic_tau_ext = 0;
	}

	hio_lte_state_set_cereg_param(&param);
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
