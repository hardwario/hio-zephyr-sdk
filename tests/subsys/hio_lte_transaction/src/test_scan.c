/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Network scan (SCAN state): CFUN=2, cells then networks, then reattach. */

#include "fsm_helpers.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern atomic_t fake_flow_scan_cells_count;
extern atomic_t fake_flow_scan_plmn_count;
extern bool fake_flow_scan_auto;
extern int fake_flow_last_cfun;
extern uint8_t fake_flow_scan_cells_act;
extern bool fake_flow_scan_rat_lte_m;
extern bool fake_flow_scan_rat_nb_iot;
extern uint32_t fake_attach_timeout_ms;
extern uint32_t fake_retry_delay_ms;
extern atomic_t fake_flow_scan_abort_count;

/* Past SCAN_SETTLE_DELAY after each RAT switch. */
#define SETTLE_MS 2200

static void fsm_down(void)
{
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(10)));
}

static void scan_after(void *fixture)
{
	ARG_UNUSED(fixture);
	fsm_down();
}

static void scan_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fsm_down();

	strcpy(g_hio_lte_config.mode, "lte-m,nb-iot");
	fake_attach_timeout_ms = 60000;
	fake_retry_delay_ms = 0;
	fake_flow_scan_auto = false;
	atomic_clear(&fake_flow_scan_cells_count);
	atomic_clear(&fake_flow_scan_plmn_count);
	atomic_clear(&fake_flow_scan_abort_count);
}

ZTEST_SUITE(hio_lte_scan, NULL, NULL, scan_before, scan_after, NULL);

ZTEST(hio_lte_scan, test_rejected_in_test_mode)
{
	g_hio_lte_config.test = true;
	zassert_equal(hio_lte_scan(HIO_LTE_SCAN_ALL), -ENOTSUP);
}

ZTEST(hio_lte_scan, test_rejected_when_disabled)
{
	zassert_equal(hio_lte_scan(HIO_LTE_SCAN_ALL), -ENODEV);
}

ZTEST(hio_lte_scan, test_rejects_invalid_mode)
{
	zassert_equal(hio_lte_scan(HIO_LTE_SCAN_CELLS + 1), -EINVAL);
}

/* From READY: LTE-M cells, NB-IoT cells, networks, then a full reattach. */
ZTEST(hio_lte_scan, test_from_ready_runs_all_steps_and_reattaches)
{
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_ALL));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_true(fake_flow_scan_rat_lte_m && !fake_flow_scan_rat_nb_iot);
	zassert_equal(hio_lte_scan(HIO_LTE_SCAN_ALL), -EALREADY);
	zassert_equal(hio_lte_wait_for_scan(K_NO_WAIT), -ETIMEDOUT);

	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_LTE);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 2);
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_true(fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);
	zassert_false(fake_flow_scan_auto);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
	zassert_ok(hio_lte_wait_for_scan(K_NO_WAIT));

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	zassert_ok(hio_lte_wait_for_connected(K_SECONDS(1)), "no reconnect after scan");
}

/* Only the RATs of the LTE mode config are searched. */
ZTEST(hio_lte_scan, test_steps_follow_mode_config)
{
	strcpy(g_hio_lte_config.mode, "nb-iot");
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_ALL));
	k_sleep(K_MSEC(50));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
}

ZTEST(hio_lte_scan, test_plmn_only_skips_cells)
{
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 0);
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
}

ZTEST(hio_lte_scan, test_cells_only_skips_plmn)
{
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_CELLS));
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(SETTLE_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 2);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 0);
	zassert_true(fsm_in("prepare"));
}

ZTEST(hio_lte_scan, test_interrupts_attach)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("attach"));

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
}

/* Plays %XSIM: 1 for PREPARE. Only there: the FSM event ring is small, and
 * flooding it drops real events. */
static bool m_sim_ticking;
static void sim_tick(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_sim_work, sim_tick);

static void sim_tick(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!m_sim_ticking) {
		return;
	}
	if (fsm_in("prepare")) {
		fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	}
	k_work_schedule(&m_sim_work, K_MSEC(200));
}

/* Third failed attempt: the scan runs within the retry delay, which resumes
 * after it without counting another attempt. */
ZTEST(hio_lte_scan, test_auto_scan_in_retry_delay)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	fake_attach_timeout_ms = 100;
	/* Above SCAN_AUTO_MIN_DELAY. */
	fake_retry_delay_ms = 20 * 60 * 1000;

	zassert_ok(hio_lte_enable(&cfg));
	m_sim_ticking = true;
	k_work_schedule(&m_sim_work, K_MSEC(20));

	/* Two zero delays (5 s CFUN=4 settle each), then the third attempt. */
	for (int i = 0; i < 300 && atomic_get(&fake_flow_scan_cells_count) == 0; i++) {
		k_sleep(K_MSEC(100));
	}
	m_sim_ticking = false;

	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1, "auto scan did not run");
	zassert_true(fake_flow_scan_auto);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(SETTLE_MS));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(SETTLE_MS));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("retry_delay"));
	zassert_equal(fake_flow_last_cfun, 4);

	int attempt;
	zassert_ok(hio_lte_get_curr_attach_info(&attempt, NULL, NULL, NULL));
	zassert_equal(attempt, 3, "scan counted as an attach attempt");
}

/* While scanning the device is not connected: a transfer is refused at once
 * (-EBUSY, not counted as a server failure) instead of timing out. */
ZTEST(hio_lte_scan, test_send_during_scan_is_refused)
{
	static uint8_t uplink[16];

	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_equal(hio_lte_wait_for_connected(K_NO_WAIT), -ETIMEDOUT,
		      "still reported as connected");

	struct hio_lte_send_recv_param param = {
		.send_buf = uplink,
		.send_len = sizeof(uplink),
		.timeout = K_SECONDS(5),
	};

	int64_t start = k_uptime_get();
	zassert_equal(hio_lte_send_recv(&param), -EBUSY);
	zassert_true(k_uptime_get() - start < 1000, "caller waited for its deadline");
}

/* Leaving the scan early publishes what it has; the request is gone after it. */
ZTEST(hio_lte_scan, test_disable_during_scan)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_ALL));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));

	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(2)));
	zassert_equal(atomic_get(&fake_flow_scan_abort_count), 1);
	zassert_ok(hio_lte_wait_for_scan(K_NO_WAIT), "waiter not released");

	/* A late cell search result must not wake the modem. */
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("disabled"));
}
