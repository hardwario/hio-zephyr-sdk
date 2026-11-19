/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Network scan (SCAN state): CFUN=1 per RAT, cells then networks, then
 * reattach. */

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
extern void (*fake_flow_scan_rat_hook)(void);
extern char fake_flow_scan_trace[];
extern uint8_t fake_flow_scan_plmn_act;

/* SCAN_ATTACH_WINDOW: with no RRC connection the cell search starts at its end. */
#define WINDOW_MS      10000
/* Past the window. */
#define PAST_WINDOW_MS (WINDOW_MS + 200)
/* SCAN_RELEASE_TIMEOUT: the longest wait for the release of the connection. */
#define RELEASE_MS     60000
/* SCAN_REGISTER_WINDOW: without a registration the network search starts at
 * its end. */
#define REGISTER_MS    60000

static void fsm_down(void)
{
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(10)));
}

/* The +CEREG: 0 of CFUN=4 in the RAT switch. */
static void unregister_on_switch(void)
{
	struct hio_lte_cereg_param param = {
		.valid = true,
		.stat = HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED,
	};

	hio_lte_state_set_cereg_param(&param);
}

static void deregister_on_switch(void)
{
	unregister_on_switch();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_DEREGISTERED);
}

static atomic_val_t cells_started(void)
{
	return atomic_get(&fake_flow_scan_cells_count);
}

static atomic_val_t plmn_started(void)
{
	return atomic_get(&fake_flow_scan_plmn_count);
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
	fake_flow_scan_rat_hook = unregister_on_switch;
	fake_flow_scan_trace[0] = '\0';
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

/* From READY, per RAT the cell search and the network search in the same
 * attach: LTE-M, then NB-IoT; then a full reattach. */
ZTEST(hio_lte_scan, test_from_ready_runs_all_steps_and_reattaches)
{
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_ALL));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_true(fake_flow_scan_rat_lte_m && !fake_flow_scan_rat_nb_iot);
	zassert_equal(hio_lte_scan(HIO_LTE_SCAN_ALL), -EALREADY);
	zassert_equal(hio_lte_wait_for_scan(K_NO_WAIT), -ETIMEDOUT);

	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_LTE);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	/* Not registered yet: the network search waits for it, in LTE-M. */
	zassert_equal(plmn_started(), 0);
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1);
	zassert_equal(fake_flow_scan_plmn_act, HIO_LTE_CEREG_PARAM_ACT_LTE);
	zassert_true(fake_flow_scan_rat_lte_m && !fake_flow_scan_rat_nb_iot);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 2);
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 2);
	zassert_equal(fake_flow_scan_plmn_act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);
	zassert_false(fake_flow_scan_auto);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
	zassert_ok(hio_lte_wait_for_scan(K_NO_WAIT));
	zassert_str_equal(fake_flow_scan_trace, "rat:m cells:7 plmn:7 rat:n cells:9 plmn:9 ");

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
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	zassert_str_equal(fake_flow_scan_trace, "rat:n cells:9 plmn:9 ");
}

/* Registered by the end of the cell search: the network search follows at
 * once, in the same attach. */
ZTEST(hio_lte_scan, test_plmn_at_once_when_registered)
{
	strcpy(g_hio_lte_config.mode, "lte-m");
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_ALL));
	k_sleep(K_MSEC(50));
	fsm_scan_registers();
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(cells_started(), 1);
	zassert_equal(plmn_started(), 0, "searched networks before the cells");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1, "did not search at once");
	zassert_equal(fake_flow_scan_plmn_act, HIO_LTE_CEREG_PARAM_ACT_LTE);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
	zassert_str_equal(fake_flow_scan_trace, "rat:m cells:7 plmn:7 ");
}

/* Per RAT: the switch, the registration, the network search. */
ZTEST(hio_lte_scan, test_plmn_only_skips_cells)
{
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 0);
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 1);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 2);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
	zassert_str_equal(fake_flow_scan_trace, "rat:m plmn:7 rat:n plmn:9 ");
}

ZTEST(hio_lte_scan, test_cells_only_skips_plmn)
{
	fsm_bring_up();

	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_CELLS));
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 2);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&fake_flow_scan_plmn_count), 0);
	zassert_true(fsm_in("prepare"));
	zassert_str_equal(fake_flow_scan_trace, "rat:m cells:7 rat:n cells:9 ");
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

	/* The end of a scan before: a request dropped by a disable. */
	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	fsm_down();
	zassert_ok(hio_lte_wait_for_scan(K_NO_WAIT));

	fake_attach_timeout_ms = 100;
	/* Above SCAN_AUTO_MIN_DELAY. */
	fake_retry_delay_ms = 20 * 60 * 1000;

	zassert_ok(hio_lte_enable(&cfg));
	m_sim_ticking = true;
	k_work_schedule(&m_sim_work, K_MSEC(20));

	/* Two zero delays (5 s CFUN=4 settle each), the third attempt, then
	 * the attach window. */
	for (int i = 0; i < 400 && atomic_get(&fake_flow_scan_cells_count) == 0; i++) {
		k_sleep(K_MSEC(100));
	}
	m_sim_ticking = false;

	zassert_equal(atomic_get(&fake_flow_scan_cells_count), 1, "auto scan did not run");
	zassert_true(fake_flow_scan_auto);
	zassert_equal(hio_lte_wait_for_scan(K_NO_WAIT), -ETIMEDOUT, "a caller would not wait");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_SECONDS(6));
	zassert_str_equal(fake_flow_scan_trace, "rat:m cells:7 plmn:7 rat:n cells:9 plmn:9 ");
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

static void start_cells_scan(void)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_CELLS));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_true(fake_flow_scan_rat_lte_m && !fake_flow_scan_rat_nb_iot);
}

/* In CFUN=1 the modem attaches, and with the RRC connection open the cell
 * search finds only the serving cell: it waits for the release, not for the
 * registration. LTE-M: the connection outlasts the attach window; NB-IoT:
 * released within it. */
ZTEST(hio_lte_scan, test_cells_wait_for_rrc_release)
{
	start_cells_scan();

	k_sleep(K_SECONDS(1));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_SECONDS(1));
	fsm_scan_registers();
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(cells_started(), 0, "searched with the RRC connection open");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 1, "not searched after the release");
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_LTE);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_SECONDS(1));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_SECONDS(2));
	zassert_equal(cells_started(), 1, "searched with the RRC connection open");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 2, "not searched at the release");
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_NBIOT);
}

/* The release never comes: the search runs anyway, with a poorer result. */
ZTEST(hio_lte_scan, test_cells_rrc_release_wait_bounded)
{
	start_cells_scan();

	k_sleep(K_SECONDS(1));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(RELEASE_MS - 500));
	zassert_equal(cells_started(), 0, "did not wait for the release");

	k_sleep(K_MSEC(700));
	zassert_equal(cells_started(), 1, "waited past the bound");
}

/* No connection opens (no coverage of the RAT, no registration): the search
 * starts when the attach window ends. */
ZTEST(hio_lte_scan, test_cells_start_after_window_without_connection)
{
	start_cells_scan();

	k_sleep(K_MSEC(WINDOW_MS - 500));
	zassert_equal(cells_started(), 0, "did not give the attach its window");

	k_sleep(K_MSEC(700));
	zassert_equal(cells_started(), 1, "not searched after the window");
}

/* A release with no connection of this step (from CFUN=4 of the switch) does
 * not start the search before the attach. */
ZTEST(hio_lte_scan, test_cells_ignore_release_without_connection)
{
	start_cells_scan();

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(500));
	zassert_equal(cells_started(), 0, "searched before the attach");
}

static void release_on_switch(void)
{
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
}

/* A connection of the previous RAT handled after the switch, which released
 * it: not the attach of the new RAT. */
ZTEST(hio_lte_scan, test_cells_ignore_connection_of_previous_rat)
{
	start_cells_scan();
	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(cells_started(), 1);

	fake_flow_scan_rat_hook = release_on_switch;
	/* Both before the FSM runs: the result, then a connection opens. */
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	zassert_equal(cells_started(), 1, "searched before the attach");

	k_sleep(K_MSEC(WINDOW_MS));
	zassert_equal(cells_started(), 2, "waited for a stale connection");
}

/* A release handled after the next connection opened (both URCs before the
 * FSM runs): still connected, the search waits for the next release. */
ZTEST(hio_lte_scan, test_cells_ignore_release_of_previous_connection)
{
	start_cells_scan();

	k_sleep(K_SECONDS(1));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_SECONDS(1));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 0, "searched with the RRC connection open");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 1, "not searched after the release");
}

/* The window ends while its CSCON 1 waits for the FSM: the timeout queued
 * behind it is stale, the search waits for the release. */
ZTEST(hio_lte_scan, test_cells_stale_window_timeout)
{
	start_cells_scan();

	k_sleep(K_MSEC(WINDOW_MS - 100));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	/* Without yielding: the window timer fires, its timeout comes second. */
	k_busy_wait(200 * USEC_PER_MSEC);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 0, "stale timeout started the search");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 1, "not searched after the release");
}

/* The connection opens as the window ends, its CSCON 1 is handled after the
 * timeout: the search waits for the release. */
ZTEST(hio_lte_scan, test_cells_window_ends_with_connection_open)
{
	start_cells_scan();

	k_sleep(K_MSEC(WINDOW_MS - 100));
	k_busy_wait(200 * USEC_PER_MSEC);
	/* The system work queue queues the window timeout first. */
	k_yield();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 0, "searched with the RRC connection open");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_equal(cells_started(), 1, "not searched after the release");
}

static void start_plmn_scan(void)
{
	fsm_bring_up();
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_true(fake_flow_scan_rat_lte_m && !fake_flow_scan_rat_nb_iot);
}

/* %COPS=? marks the network of the registration as current: it waits for
 * the attach to register (an open RRC connection does not matter). */
ZTEST(hio_lte_scan, test_plmn_waits_for_registration)
{
	start_plmn_scan();

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_SECONDS(5));
	zassert_equal(plmn_started(), 0, "searched before the registration");

	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1, "not searched after the registration");
}

/* No registration (reject, no coverage): the search starts when the window
 * ends. */
ZTEST(hio_lte_scan, test_plmn_start_after_window_without_registration)
{
	start_plmn_scan();

	k_sleep(K_MSEC(REGISTER_MS - 500));
	zassert_equal(plmn_started(), 0, "did not give the registration its window");

	k_sleep(K_MSEC(700));
	zassert_equal(plmn_started(), 1, "not searched after the window");
}

/* A registration of the previous RAT handled after the switch, which ended
 * it (+CEREG: 0): not the registration of the next RAT. */
ZTEST(hio_lte_scan, test_plmn_ignore_registration_of_previous_rat)
{
	start_plmn_scan();
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1);

	fake_flow_scan_rat_hook = deregister_on_switch;
	/* Both before the FSM runs: the end of the search, then the LTE-M
	 * registration. */
	fsm_set_psm(0);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	zassert_equal(plmn_started(), 1, "searched before the registration");

	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 2, "not searched after the registration");
}

/* After a switch the registration still reported is of the previous RAT
 * (its +CEREG: 0 not handled yet): the search waits for one. */
ZTEST(hio_lte_scan, test_plmn_after_switch_waits_for_registration)
{
	start_plmn_scan();
	fake_flow_scan_rat_hook = NULL;
	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_SECONDS(1));
	zassert_true(!fake_flow_scan_rat_lte_m && fake_flow_scan_rat_nb_iot);
	zassert_equal(plmn_started(), 1, "searched before the registration");

	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 2, "not searched after the registration");
}

/* A scan requested or running refuses a measurement: it would wait for the
 * reattach after it. */
ZTEST(hio_lte_scan, test_ncellmeas_refused_while_scanning)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("prepare"));
	zassert_ok(hio_lte_scan(HIO_LTE_SCAN_PLMN));
	zassert_equal(hio_lte_schedule_ncellmeas(), -EBUSY);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"));
	zassert_equal(hio_lte_schedule_ncellmeas(), -EBUSY);
}

/* The window ends while the registration waits for the FSM: the timeout
 * queued behind it is stale, the search starts once. */
ZTEST(hio_lte_scan, test_plmn_stale_window_timeout)
{
	start_plmn_scan();

	k_sleep(K_MSEC(REGISTER_MS - 100));
	fsm_scan_registers();
	/* Without yielding: the window timer fires, its timeout comes second. */
	k_busy_wait(200 * USEC_PER_MSEC);
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1, "not searched once");
	zassert_true(fsm_in("scan"), "stale timeout ended the search");
}

/* A late result (of a measurement the scan stopped) before the search does
 * not end the step. */
ZTEST(hio_lte_scan, test_cells_ignore_result_before_search)
{
	start_cells_scan();

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
	zassert_true(fake_flow_scan_rat_lte_m && !fake_flow_scan_rat_nb_iot, "step ended");

	k_sleep(K_MSEC(PAST_WINDOW_MS));
	zassert_equal(cells_started(), 1);
	zassert_equal(fake_flow_scan_cells_act, HIO_LTE_CEREG_PARAM_ACT_LTE);
}

/* Nor does a %COPS=? completion before the network search. */
ZTEST(hio_lte_scan, test_plmn_ignore_done_before_search)
{
	start_plmn_scan();

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("scan"), "step ended");

	fsm_scan_registers();
	k_sleep(K_MSEC(50));
	zassert_equal(plmn_started(), 1);
}
