/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Neighbour cell measurement on request (NCELLMEAS state). */

#include "fsm_helpers.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern atomic_t fake_flow_ncellmeas_count;
extern atomic_t fake_flow_send_count;
extern int fake_flow_last_cfun;
extern void fake_flow_ncellmeas_resp(const char *resp);

static atomic_t m_done_count;
static int m_wait_at_start;

static void done_handler(struct hio_lte_cb *cb, enum hio_lte_event event)
{
	ARG_UNUSED(cb);

	if (event == HIO_LTE_EVENT_NCELLMEAS_DONE) {
		atomic_inc(&m_done_count);
	}
}

static struct hio_lte_cb m_cb = {.handler = done_handler};

static void fsm_down(void)
{
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(10)));
}

/* The result URC, as the flow reports it. */
static void urc(void)
{
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	k_sleep(K_MSEC(50));
}

static void measured(void)
{
	urc();
	zassert_true(fsm_in("ready"));
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "waiter not released");
	zassert_equal(atomic_get(&m_done_count), 1);
}

static void *ncellmeas_setup(void)
{
	/* Before any request of these tests. */
	m_wait_at_start = hio_lte_wait_for_ncellmeas(K_NO_WAIT);
	zassert_ok(hio_lte_add_callback(&m_cb));

	return NULL;
}

static void ncellmeas_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* With CONFIG_HIO_LTE_NCELLMEAS, other suites leave the one of their
	 * enable. */
	fsm_end_ncellmeas();
	fsm_down();
	fsm_set_psm(0);
	atomic_clear(&m_done_count);
	atomic_clear(&fake_flow_ncellmeas_count);
}

static void ncellmeas_after(void *fixture)
{
	ARG_UNUSED(fixture);

	fsm_end_ncellmeas();
	fsm_down();
}

ZTEST_SUITE(hio_lte_ncellmeas, NULL, ncellmeas_setup, ncellmeas_before, ncellmeas_after, NULL);

ZTEST(hio_lte_ncellmeas, test_nothing_requested)
{
	/* Other suites have enabled LTE, which requests one. */
	Z_TEST_SKIP_IFDEF(CONFIG_HIO_LTE_NCELLMEAS);

	zassert_ok(m_wait_at_start);
}

ZTEST(hio_lte_ncellmeas, test_rejected_in_test_mode)
{
	g_hio_lte_config.test = true;
	zassert_equal(hio_lte_schedule_ncellmeas(), -ENOTSUP);
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
}

/* Registered and the RRC connection idle: it starts at once. */
ZTEST(hio_lte_ncellmeas, test_from_ready_measures_at_once)
{
	fsm_bring_up();
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));

	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
	zassert_equal(hio_lte_wait_for_ncellmeas(K_NO_WAIT), -ETIMEDOUT);

	/* Served by the running one, no second measurement after it. */
	zassert_equal(hio_lte_schedule_ncellmeas(), -EALREADY);
	k_sleep(K_MSEC(50));
	measured();
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1, "measured twice");
}

/* With the RRC connection open it waits for its release. */
ZTEST(hio_lte_ncellmeas, test_waits_for_rrc_release)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));

	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	measured();
}

/* Requested while busy (here attaching): it starts once ready. */
ZTEST(hio_lte_ncellmeas, test_requested_while_attaching)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("attach"));

	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("attach"));

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	measured();
}

/* Requested in PSM sleep: the modem measures without waiting for the next
 * connection. */
static void measuring_from_sleep(void)
{
	fsm_bring_up();
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_XMODEMSLEEP);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("sleep"));

	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
}

ZTEST(hio_lte_ncellmeas, test_from_psm_sleep_measures_at_once)
{
	measuring_from_sleep();

	/* The command accepted: the result follows as a URC. */
	fake_flow_ncellmeas_resp("OK\r\n");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"), "ended by OK");
	measured();
}

/* No PSM granted: the ready state switches the radio off (CFUN=4). */
static void radio_off(void)
{
	fsm_set_psm(-1);
	fsm_bring_up();
	/* The radio stays on for a requested measurement. */
	fsm_boot_ncellmeas();
	atomic_clear(&m_done_count);
	atomic_clear(&fake_flow_ncellmeas_count);
	for (int i = 0; i < 40 && fake_flow_last_cfun != 4; i++) {
		k_sleep(K_MSEC(50));
	}
	zassert_equal(fake_flow_last_cfun, 4);
}

/* With the radio off it attaches first; the release of that connection starts
 * the measurement. */
static void attaches_then_measures(void)
{
	zassert_equal(fake_flow_last_cfun, 1);
	zassert_true(fsm_in("attach"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	zassert_ok(hio_lte_wait_for_connected(K_SECONDS(1)));
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	measured();
}

ZTEST(hio_lte_ncellmeas, test_from_sleep_with_radio_off_attaches)
{
	radio_off();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_XMODEMSLEEP);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("sleep"));

	zassert_ok(hio_lte_schedule_ncellmeas());
	k_sleep(K_MSEC(50));
	attaches_then_measures();
}

/* Requested after CFUN=4, before the sleep state: no measurement with the radio
 * off, the sleep state wakes it. */
ZTEST(hio_lte_ncellmeas, test_from_ready_with_radio_off_attaches)
{
	radio_off();
	zassert_true(fsm_in("ready"));

	zassert_ok(hio_lte_schedule_ncellmeas());
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_XMODEMSLEEP);
	k_sleep(K_MSEC(50));
	attaches_then_measures();
}

/* Requested in READY with the connection idle: it starts at once. */
static void measuring_from_ready(void)
{
	fsm_bring_up();
	k_sleep(K_MSEC(50));
	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
}

/* No URC within the watchdog: the request stays for the next connection and
 * is not retried in a loop; asked again, it measures again. */
ZTEST(hio_lte_ncellmeas, test_watchdog_keeps_request)
{
	measuring_from_ready();

	k_sleep(K_SECONDS(61));
	zassert_true(fsm_in("ready"));
	zassert_equal(hio_lte_wait_for_ncellmeas(K_NO_WAIT), -ETIMEDOUT, "request dropped");
	zassert_equal(atomic_get(&m_done_count), 0);

	k_sleep(K_SECONDS(2));
	zassert_true(fsm_in("ready"), "measured again without a request");
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);

	zassert_equal(hio_lte_schedule_ncellmeas(), -EALREADY);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 2);
	measured();
}

/* The request event without a request starts nothing. */
ZTEST(hio_lte_ncellmeas, test_stray_request_event)
{
	fsm_bring_up();
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS_REQ);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_XMODEMSLEEP);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("sleep"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS_REQ);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("sleep"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0);
}

/* A request left by the watchdog runs at the release of the next connection. */
ZTEST(hio_lte_ncellmeas, test_after_watchdog_measures_at_release)
{
	measuring_from_ready();
	k_sleep(K_SECONDS(61));
	zassert_true(fsm_in("ready"));

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"), "not started by the release");
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 2);
	measured();
}

/* Asked again while it runs: served by the running one, so after the watchdog
 * it waits for the next connection like any other. */
ZTEST(hio_lte_ncellmeas, test_repeat_while_running_not_rearmed)
{
	measuring_from_ready();
	k_sleep(K_SECONDS(10));
	zassert_equal(hio_lte_schedule_ncellmeas(), -EALREADY);

	k_sleep(K_SECONDS(51));
	zassert_true(fsm_in("ready"));
	k_sleep(K_SECONDS(2));
	zassert_true(fsm_in("ready"), "measured again without a request");
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
}

/* The result arrives just after the watchdog; without PSM the radio then goes
 * off as after any measurement. */
ZTEST(hio_lte_ncellmeas, test_late_result_ends_request)
{
	measuring_from_ready();
	fsm_set_psm(-1);
	k_sleep(K_SECONDS(61));
	zassert_true(fsm_in("ready"));
	zassert_equal(hio_lte_wait_for_ncellmeas(K_NO_WAIT), -ETIMEDOUT);
	zassert_equal(fake_flow_last_cfun, 1);

	measured();
	k_sleep(K_SECONDS(1));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
	zassert_equal(fake_flow_last_cfun, 4, "radio left on");
}

/* The modem refuses the command (it does in CFUN=2): no result follows, the
 * request ends at once as failed. */
ZTEST(hio_lte_ncellmeas, test_refused_ends_request)
{
	hio_lte_state_set_ncellmeas_param(&(struct hio_lte_ncellmeas_param){0});
	measuring_from_ready();

	fake_flow_ncellmeas_resp("ERROR\r\n");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "waiter not released");
	zassert_equal(atomic_get(&m_done_count), 1);

	struct hio_lte_ncellmeas_param param;

	zassert_ok(hio_lte_get_ncellmeas_param(&param));
	zassert_true(param.valid);
	zassert_equal(param.status, 1, "not stored as failed");

	k_sleep(K_SECONDS(1));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1, "tried again");
}

static K_THREAD_STACK_DEFINE(m_thread_stack, 2048);
static struct k_thread m_thread;

static uint8_t m_uplink[8];
static struct hio_lte_send_recv_param m_send_param;
static int m_send_ret;

static void send_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	m_send_ret = hio_lte_send_recv(&m_send_param);
}

/* A transaction from a thread of its own. */
static void send_start(k_timeout_t timeout)
{
	m_send_param = (struct hio_lte_send_recv_param){
		.send_buf = m_uplink,
		.send_len = sizeof(m_uplink),
		.timeout = timeout,
	};
	m_send_ret = 1;
	atomic_clear(&fake_flow_send_count);
	k_thread_create(&m_thread, m_thread_stack, K_THREAD_STACK_SIZEOF(m_thread_stack),
			send_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	k_sleep(K_MSEC(50));
}

/* The network grants the connection and the uplink goes out. */
static void send_done(void)
{
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	zassert_ok(k_thread_join(&m_thread, K_SECONDS(1)), "transaction did not end");
	zassert_ok(m_send_ret);
}

/* A transaction waits for the running measurement. */
ZTEST(hio_lte_ncellmeas, test_transaction_waits_for_measurement)
{
	measuring_from_sleep();
	send_start(K_SECONDS(120));
	k_sleep(K_SECONDS(5));
	zassert_true(fsm_in("ncellmeas"));
	zassert_equal(atomic_get(&fake_flow_send_count), 0, "sent while measuring");

	urc();
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
	zassert_equal(atomic_get(&fake_flow_send_count), 1, "not sent after it");
	send_done();
}

/* The deadline of a waiting transaction does not abort the measurement. */
ZTEST(hio_lte_ncellmeas, test_transaction_deadline_keeps_measurement)
{
	measuring_from_sleep();
	send_start(K_SECONDS(5));
	zassert_ok(k_thread_join(&m_thread, K_SECONDS(10)));
	zassert_equal(m_send_ret, -ETIMEDOUT);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"), "measurement aborted");

	measured();
	k_sleep(K_SECONDS(1));
	zassert_equal(atomic_get(&fake_flow_send_count), 0, "abandoned transaction sent");
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
}

/* A transaction and a request both pending on entering READY: the transaction
 * goes first, the release of its connection starts the measurement. */
ZTEST(hio_lte_ncellmeas, test_transaction_before_measurement)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("attach"));
	send_start(K_SECONDS(120));
	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("send"));
	zassert_equal(atomic_get(&fake_flow_send_count), 1);
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0, "measured first");
	send_done();
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	measured();
}

static int m_waiter_ret;

static void waiter_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!hio_lte_wait_for_ncellmeas(K_SECONDS(5))) {
		m_waiter_ret = hio_lte_schedule_ncellmeas();
	}
}

/* The done bit comes while the request is still set: a request made at that
 * moment (here by a waiter it releases, which runs before the FSM thread goes
 * on) is served by this result, it never stays pending with the done bit set. */
ZTEST(hio_lte_ncellmeas, test_request_at_result_served_by_it)
{
	measuring_from_ready();
	m_waiter_ret = 1;
	k_thread_create(&m_thread, m_thread_stack, K_THREAD_STACK_SIZEOF(m_thread_stack),
			waiter_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	k_sleep(K_MSEC(50));

	urc();
	zassert_ok(k_thread_join(&m_thread, K_SECONDS(1)));
	zassert_equal(m_waiter_ret, -EALREADY);
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
}

/* CONFIG_HIO_LTE_NCELLMEAS: every enable requests one, which waits for the
 * release of the RRC connection. */
ZTEST(hio_lte_ncellmeas, test_enable_requests_one)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_HIO_LTE_NCELLMEAS);

	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
	fsm_bring_up();
	zassert_equal(hio_lte_wait_for_ncellmeas(K_NO_WAIT), -ETIMEDOUT, "done before it ran");
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"));
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0, "measured before the release");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"));
	measured();
}

/* CONFIG_HIO_LTE_NCELLMEAS: a request repeated while one ran does not make the
 * one of the next enable urgent. */
ZTEST(hio_lte_ncellmeas, test_repeat_not_left_for_next_enable)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_HIO_LTE_NCELLMEAS);

	measuring_from_ready();
	zassert_equal(hio_lte_schedule_ncellmeas(), -EALREADY);
	k_sleep(K_MSEC(50));
	measured();

	fsm_down();
	fsm_bring_up();
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("ready"), "measured before the release");
	zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 1);
}

/* A disable drops a request and releases its waiter; it is not left for the
 * next enable. */
ZTEST(hio_lte_ncellmeas, test_disable_drops_request)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_equal(hio_lte_schedule_ncellmeas(), FIRST_NCELLMEAS_RET);
	zassert_equal(hio_lte_wait_for_ncellmeas(K_NO_WAIT), -ETIMEDOUT);

	fsm_down();
	zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT), "waiter not released");
	zassert_equal(atomic_get(&m_done_count), 0, "reported as measured");

	if (!IS_ENABLED(CONFIG_HIO_LTE_NCELLMEAS)) {
		fsm_bring_up();
		k_sleep(K_SECONDS(1));
		zassert_true(fsm_in("ready"));
		zassert_equal(atomic_get(&fake_flow_ncellmeas_count), 0, "left for the enable");
		zassert_ok(hio_lte_wait_for_ncellmeas(K_NO_WAIT));
	}
}
