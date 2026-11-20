/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* A caller of hio_lte_send_recv() that gives up at its deadline.
 *
 * It must not disturb the state the FSM is in (an attach, a retry delay, the
 * error state keep their own timers), it must not return while the FSM is still
 * in the send or the recv that uses its buffers, and the result is decided by
 * who ended the transaction, not by the clock. */

#include "fsm_helpers.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <string.h>

extern atomic_t fake_flow_send_count;
extern uint32_t fake_flow_send_block_ms;
extern uint32_t fake_flow_send_busy_ms;
extern int fake_flow_send_ret;
extern uint8_t fake_flow_wire[64];
extern size_t fake_flow_wire_len;
extern atomic_t fake_flow_in_io;
extern int64_t fake_flow_io_exit_ms;
extern k_tid_t fake_flow_fsm_thread;
extern size_t fake_flow_recv_len;
extern uint32_t fake_flow_recv_block_ms;
extern uint8_t fake_flow_recv_fill;
extern int fake_flow_sndtimeo_sec;
extern uint32_t fake_attach_timeout_ms;
extern uint32_t fake_retry_delay_ms;
extern void (*fake_rtc_hook)(void);
extern uint32_t fake_flow_check_ms;
extern uint32_t fake_flow_open_ms;
extern uint32_t fake_flow_coneval_ms;
extern struct k_mutex m_event_rb_lock;
extern struct k_work_delayable m_timeout_work;

#define PAYLOAD_LEN 32
#define IO_BLOCK_MS 400
#define CANARY      0xc3

static uint8_t m_payload[PAYLOAD_LEN];

static K_THREAD_STACK_DEFINE(m_caller_stack, 2048);
static struct k_thread m_caller_thread;
static struct hio_lte_send_recv_param m_caller_param;
static int m_caller_ret;
static int64_t m_caller_ret_ms;

static void caller_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	m_caller_ret = hio_lte_send_recv(&m_caller_param);
	m_caller_ret_ms = k_uptime_get();
}

static void caller_start(int prio)
{
	k_thread_create(&m_caller_thread, m_caller_stack, K_THREAD_STACK_SIZEOF(m_caller_stack),
			caller_entry, NULL, NULL, NULL, prio, 0, K_NO_WAIT);
}

static void caller_join(void)
{
	zassert_ok(k_thread_join(&m_caller_thread, K_SECONDS(10)), "caller did not return");
}

/* The state in which the FSM is held up just before its send or recv. */
static const char *m_hold_state;

static void hold_in_state(void)
{
	if (m_hold_state && fsm_in(m_hold_state)) {
		k_sleep(K_MSEC(IO_BLOCK_MS));
	}
}

static void fakes_reset(void)
{
	fake_rtc_hook = NULL;
	m_hold_state = NULL;
	fake_flow_check_ms = 0;
	fake_flow_open_ms = 0;
	fake_flow_coneval_ms = 0;
	if (fake_flow_fsm_thread) {
		k_thread_priority_set(fake_flow_fsm_thread, K_LOWEST_APPLICATION_THREAD_PRIO);
	}

	fake_flow_send_block_ms = 0;
	fake_flow_send_busy_ms = 0;
	fake_flow_send_ret = 0;
	fake_flow_wire_len = 0;
	memset(fake_flow_wire, 0, sizeof(fake_flow_wire));
	fake_flow_io_exit_ms = 0;
	fake_flow_recv_len = 0;
	fake_flow_recv_block_ms = 0;
	fake_flow_recv_fill = 0x5a;
	fake_flow_sndtimeo_sec = -1;
	fake_attach_timeout_ms = 60000;
	fake_retry_delay_ms = 0;
	atomic_clear(&fake_flow_send_count);
	memset(m_payload, 'P', sizeof(m_payload));
}

static void fsm_down(void)
{
	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(10)));
}

static void txn_abort_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fsm_down();
	fakes_reset();
	fsm_set_psm(0);
}

static void txn_abort_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* No send or recv left running into the next test. */
	WAIT_FOR(!atomic_get(&fake_flow_in_io), 5000000, k_msleep(10));
	fakes_reset();
	fsm_end_ncellmeas();
	fsm_down();
}

ZTEST_SUITE(hio_lte_txn_abort, NULL, NULL, txn_abort_before, txn_abort_after, NULL);

static void fsm_to_attach(void)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("attach"));
}

/* Connected, so a send goes on to the recv or ends the transaction. */
static void fsm_to_ready_connected(void)
{
	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
}

static uint32_t uplink_errors(void)
{
	struct hio_lte_metrics metrics;

	zassert_ok(hio_lte_get_metrics(&metrics));
	return metrics.uplink_errors;
}

/* The transaction after an abandoned one goes through on its own. */
static void next_transaction_runs_clean(int sends_before)
{
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_SECONDS(5),
	};

	fake_flow_send_block_ms = 0;
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	k_sleep(K_MSEC(50));
	zassert_ok(hio_lte_send_recv(&param));
	zassert_equal(atomic_get(&fake_flow_send_count), sends_before + 1);
}

static const char *state_now(void)
{
	const char *state = "?";

	hio_lte_get_fsm_state(&state);
	return state;
}

/* Fill the event queue while the FSM thread is busy, so that the next event
 * posted is lost. XTIME means nothing to any state. */
static void fill_event_queue(void)
{
	for (int i = 0; i < 8; i++) {
		fake_flow_event_cb(HIO_LTE_FSM_EVENT_XTIME);
	}
}

/* Call post() just before the timer of the FSM state is due and keep the CPU
 * from the FSM thread across the expiry, from a thread above it and below the
 * system work queue, which runs the timer: what post() queues is ahead of the
 * TIMEOUT, as when the FSM thread runs late. */
static void post_before_timer_expiry(const char *state, void (*post)(void))
{
	int prio = k_thread_priority_get(k_current_get());
	k_ticks_t left = k_work_delayable_remaining_get(&m_timeout_work);

	zassert_true(left > 3, "no timer pending (%d ticks)", (int)left);
	k_sleep(K_TICKS(left - 2));
	zassert_true(fsm_in(state), "in %s", state_now());
	zassert_true(k_work_delayable_remaining_get(&m_timeout_work) > 0, "fired already");

	k_thread_priority_set(k_current_get(), K_PRIO_PREEMPT(1));
	post();
	k_busy_wait(30 * USEC_PER_MSEC);
	k_thread_priority_set(k_current_get(), prio);
}

/* The attach keeps its own timer: it is neither counted as failed nor started
 * again, and the caller returns on its deadline. */
ZTEST(hio_lte_txn_abort, test_give_up_in_attach)
{
	struct hio_lte_metrics before;
	struct hio_lte_metrics after;
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_MSEC(500),
	};

	fsm_to_attach();
	zassert_ok(hio_lte_get_metrics(&before));

	int64_t start = k_uptime_get();

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);

	int64_t took = k_uptime_get() - start;

	zassert_true(took >= 500 && took <= 600, "returned after %lld ms", took);

	k_sleep(K_MSEC(200));
	zassert_ok(hio_lte_get_metrics(&after));
	zassert_true(fsm_in("attach"), "the attach was cut short");
	zassert_equal(after.attach_count, before.attach_count, "the attach was started again");
	zassert_equal(after.attach_fail_count, before.attach_fail_count,
		      "the attach was counted as failed");

	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	zassert_ok(hio_lte_wait_for_connected(K_SECONDS(1)));
	k_sleep(K_MSEC(100));
	zassert_equal(atomic_get(&fake_flow_send_count), 0, "the abandoned data was sent");
}

/* The retry delay runs to its own end. */
ZTEST(hio_lte_txn_abort, test_give_up_in_retry_delay)
{
	int attempt = 0;
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_SECONDS(1),
	};

	fsm_bring_up();
	fsm_boot_ncellmeas();
	k_sleep(K_MSEC(50));
	fake_attach_timeout_ms = 100;
	/* Below the delay that starts an automatic scan. */
	fake_retry_delay_ms = 60 * 1000;
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

	zassert_true(fsm_in("retry_delay"));
	/* Past its CFUN=4 settle, in the delay itself. */
	k_sleep(K_SECONDS(6));

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);
	k_sleep(K_SECONDS(5));
	zassert_true(fsm_in("retry_delay"), "the retry delay was cut short");

	k_sleep(K_SECONDS(60));
	zassert_false(fsm_in("retry_delay"), "the retry delay did not end on its own timer");
	zassert_equal(atomic_get(&fake_flow_send_count), 0, "the abandoned data was sent");
}

/* The error state keeps its timer (5 s after a failed send with a working
 * socket) and the abandoned data is not sent again after it. */
ZTEST(hio_lte_txn_abort, test_give_up_in_error)
{
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_SECONDS(2),
	};

	fsm_bring_up();
	fake_flow_send_ret = -EIO;

	int64_t start = k_uptime_get();

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);
	zassert_equal(atomic_get(&fake_flow_send_count), 1);

	k_sleep(K_MSEC(1000));
	zassert_true(fsm_in("error"), "the error state was cut short");

	k_sleep(K_TIMEOUT_ABS_MS(start + 5500));
	zassert_true(fsm_in("ready"), "the error state did not end on its own timer");
	k_sleep(K_MSEC(500));
	zassert_equal(atomic_get(&fake_flow_send_count), 1, "the abandoned data was sent again");
}

/* The caller gives up while the send blocks (waiting for RRC): it returns only
 * after the send, which reads its buffer until it ends, the abort is not an
 * uplink error, and the next transaction runs clean. */
ZTEST(hio_lte_txn_abort, test_give_up_while_send_blocks)
{
	uint8_t buf[PAYLOAD_LEN];
	struct hio_lte_send_recv_param param = {
		.send_buf = buf,
		.send_len = sizeof(buf),
		.timeout = K_MSEC(100),
	};

	memset(buf, 'P', sizeof(buf));
	fsm_bring_up();
	uint32_t errors = uplink_errors();
	fake_flow_send_block_ms = IO_BLOCK_MS;

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);
	int64_t ret_ms = k_uptime_get();

	/* The caller reuses its buffer. */
	memset(buf, CANARY, sizeof(buf));
	k_sleep(K_MSEC(2 * IO_BLOCK_MS));

	zassert_true(ret_ms >= fake_flow_io_exit_ms, "returned %lld ms before the send ended",
		     fake_flow_io_exit_ms - ret_ms);
	zassert_equal(fake_flow_wire_len, sizeof(buf));
	zassert_mem_equal(fake_flow_wire, m_payload, sizeof(buf),
			  "the send read the caller's buffer after the return");
	zassert_true(fsm_in("ready"));
	zassert_equal(uplink_errors(), errors, "the abort was counted as an uplink error");

	next_transaction_runs_clean(1);
}

/* The caller gives up before the send starts: the FSM skips it, as the caller
 * may already reuse its buffer. */
ZTEST(hio_lte_txn_abort, test_give_up_before_send_skips_it)
{
	uint8_t buf[PAYLOAD_LEN];
	struct hio_lte_send_recv_param param = {
		.send_buf = buf,
		.send_len = sizeof(buf),
		.timeout = K_MSEC(100),
	};

	struct hio_lte_metrics before, after;

	memset(buf, 'P', sizeof(buf));
	fsm_to_ready_connected();
	zassert_ok(hio_lte_get_metrics(&before));
	m_hold_state = "send";
	fake_rtc_hook = hold_in_state;

	int64_t start = k_uptime_get();

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);

	int64_t took = k_uptime_get() - start;

	memset(buf, CANARY, sizeof(buf));
	k_sleep(K_MSEC(2 * IO_BLOCK_MS));

	zassert_true(took < IO_BLOCK_MS, "returned after %lld ms", took);
	zassert_equal(atomic_get(&fake_flow_send_count), 0, "the abandoned data was sent");
	zassert_true(fsm_in("ready"));

	/* Neither an uplink nor an uplink error. */
	zassert_ok(hio_lte_get_metrics(&after));
	zassert_equal(after.uplink_count, before.uplink_count, "counted as an uplink");
	zassert_equal(after.uplink_bytes, before.uplink_bytes);
	zassert_equal(after.uplink_errors, before.uplink_errors);
}

/* The caller gives up before the recv starts: the FSM skips it, as the caller
 * may already reuse its buffer. */
ZTEST(hio_lte_txn_abort, test_give_up_before_recv_skips_it)
{
	uint8_t rx[64];
	uint8_t canary[sizeof(rx)];
	size_t len = 777;
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		.timeout = K_MSEC(100),
	};

	struct hio_lte_metrics before, after;

	fsm_to_ready_connected();
	zassert_ok(hio_lte_get_metrics(&before));
	fake_flow_recv_len = 10;
	m_hold_state = "receive";
	fake_rtc_hook = hold_in_state;

	int64_t start = k_uptime_get();

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);

	int64_t took = k_uptime_get() - start;

	memset(rx, CANARY, sizeof(rx));
	memset(canary, CANARY, sizeof(canary));
	k_sleep(K_MSEC(2 * IO_BLOCK_MS));

	zassert_true(took < IO_BLOCK_MS, "returned after %lld ms", took);
	zassert_mem_equal(rx, canary, sizeof(rx), "the recv wrote the buffer after the return");
	zassert_equal(len, 777, "recv_len written after the give-up");
	zassert_true(fsm_in("ready"));

	zassert_ok(hio_lte_get_metrics(&after));
	zassert_equal(after.downlink_count, before.downlink_count, "counted as a downlink");
}

/* The caller gives up while the send state waits for CSCON 1: SEND_ABORT goes
 * to the ready state at once, it is not an uplink error, and the next
 * transaction runs clean. */
ZTEST(hio_lte_txn_abort, test_give_up_waiting_for_cscon)
{
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_MSEC(300),
	};

	fsm_bring_up();
	uint32_t errors = uplink_errors();

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));
	zassert_equal(uplink_errors(), errors, "the abort was counted as an uplink error");

	next_transaction_runs_clean(1);
}

/* The caller gives up while the ready state checks the socket for its send, and
 * the next caller publishes before the FSM moves on: the FSM sends the next
 * transaction, and the abort of the previous one does not cut it short. */
ZTEST(hio_lte_txn_abort, test_abort_spares_next_transaction)
{
	uint8_t buf[PAYLOAD_LEN];
	struct hio_lte_send_recv_param param = {
		.send_buf = buf,
		.send_len = sizeof(buf),
		.timeout = K_SECONDS(5),
	};

	memset(buf, 'N', sizeof(buf));
	fsm_to_ready_connected();
	fake_flow_check_ms = IO_BLOCK_MS;
	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_MSEC(100),
	};
	caller_start(K_PRIO_PREEMPT(0));
	caller_join();
	zassert_equal(m_caller_ret, -ETIMEDOUT);

	zassert_ok(hio_lte_send_recv(&param));
	k_sleep(K_MSEC(100));
	zassert_equal(atomic_get(&fake_flow_send_count), 1, "sent %d times",
		      (int)atomic_get(&fake_flow_send_count));
	zassert_mem_equal(fake_flow_wire, buf, sizeof(buf));
}

/* The caller gives up during the recv: it returns only after the recv, so the
 * reply the FSM writes cannot land in the buffer the caller reuses. */
ZTEST(hio_lte_txn_abort, test_give_up_during_recv_keeps_buffer)
{
	uint8_t rx[64];
	uint8_t canary[sizeof(rx)];
	size_t len = 777;
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		.timeout = K_MSEC(100),
	};

	fsm_to_ready_connected();
	fake_flow_recv_len = 10;
	fake_flow_recv_block_ms = IO_BLOCK_MS;

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);
	int64_t ret_ms = k_uptime_get();

	/* The caller reuses its buffer. */
	memset(rx, CANARY, sizeof(rx));
	memset(canary, CANARY, sizeof(canary));
	k_sleep(K_MSEC(2 * IO_BLOCK_MS));

	zassert_true(ret_ms >= fake_flow_io_exit_ms, "returned %lld ms before the recv ended",
		     fake_flow_io_exit_ms - ret_ms);
	zassert_mem_equal(rx, canary, sizeof(rx), "the recv wrote the buffer after the return");
	zassert_equal(len, 777, "recv_len written after the give-up");
}

/* SNDTIMEO follows the time left to the caller's deadline, not its whole
 * timeout; without a deadline it is the ceiling. */
ZTEST(hio_lte_txn_abort, test_sndtimeo_from_time_left)
{
	fsm_to_attach();
	/* The send completes at once. */
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);

	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_SECONDS(10),
	};
	caller_start(K_PRIO_PREEMPT(0));

	k_sleep(K_SECONDS(4));
	zassert_equal(atomic_get(&fake_flow_send_count), 0);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	caller_join();
	zassert_ok(m_caller_ret);
	zassert_equal(fake_flow_sndtimeo_sec, 6, "SNDTIMEO %d s, 6 s left", fake_flow_sndtimeo_sec);

	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_FOREVER,
	};

	zassert_ok(hio_lte_send_recv(&param));
	zassert_equal(fake_flow_sndtimeo_sec, 120);
}

/* The FSM ends the transaction after the deadline, before the caller (of lower
 * priority) gets to run: the result is the FSM's, not -ETIMEDOUT. */
ZTEST(hio_lte_txn_abort, test_end_after_deadline_counts)
{
	fsm_to_ready_connected();
	zassert_not_null(fake_flow_fsm_thread);
	k_thread_priority_set(fake_flow_fsm_thread, K_LOWEST_APPLICATION_THREAD_PRIO - 1);
	fake_flow_send_busy_ms = IO_BLOCK_MS;

	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_MSEC(200),
	};
	int64_t start = k_uptime_get();

	caller_start(K_LOWEST_APPLICATION_THREAD_PRIO);
	caller_join();

	zassert_true(fake_flow_io_exit_ms - start >= 200, "the send ended before the deadline");
	zassert_equal(m_caller_ret, 0, "got %d", m_caller_ret);
}

/* The FSM finds the transaction gone and queues READY before the caller (of
 * lower priority, held up at the event queue) queues SEND_ABORT: the skipped
 * send is still no uplink error. */
ZTEST(hio_lte_txn_abort, test_skip_before_abort_not_counted)
{
	struct hio_lte_metrics before, after;

	fsm_to_ready_connected();
	zassert_ok(hio_lte_get_metrics(&before));
	zassert_not_null(fake_flow_fsm_thread);
	k_thread_priority_set(fake_flow_fsm_thread, K_LOWEST_APPLICATION_THREAD_PRIO - 1);
	fake_flow_check_ms = IO_BLOCK_MS;
	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.timeout = K_MSEC(100),
	};
	caller_start(K_LOWEST_APPLICATION_THREAD_PRIO);

	/* Held across the caller's give-up and the FSM's take; on the unlock
	 * the FSM, of higher priority, gets the queue first. */
	k_sleep(K_MSEC(50));
	zassert_ok(k_mutex_lock(&m_event_rb_lock, K_FOREVER));
	k_sleep(K_MSEC(IO_BLOCK_MS));
	zassert_ok(k_mutex_unlock(&m_event_rb_lock));
	caller_join();
	k_sleep(K_MSEC(100));

	zassert_equal(m_caller_ret, -ETIMEDOUT);
	zassert_equal(atomic_get(&fake_flow_send_count), 0);
	zassert_true(fsm_in("ready"), "in %s", state_now());
	zassert_ok(hio_lte_get_metrics(&after));
	zassert_equal(after.uplink_errors, before.uplink_errors, "counted as an uplink error");
	zassert_equal(after.uplink_count, before.uplink_count);
}

/* The RECV that ends the receive state is lost: its guard moves the FSM on,
 * timed from the end of the recv (longer than the guard). */
ZTEST(hio_lte_txn_abort, test_receive_guard)
{
	static uint8_t rx[64];
	static size_t len;

	fsm_to_ready_connected();
	fake_flow_recv_len = 10;
	fake_flow_recv_block_ms = 6000;
	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		.timeout = K_SECONDS(10),
	};
	caller_start(K_PRIO_PREEMPT(0));

	k_sleep(K_MSEC(100));
	zassert_true(fsm_in("receive"));
	fill_event_queue();
	caller_join();
	zassert_ok(m_caller_ret, "got %d", m_caller_ret);

	k_sleep(K_MSEC(IO_BLOCK_MS));
	zassert_true(fsm_in("receive"), "the RECV was not lost: %s", state_now());
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("ready"), "stuck in %s", state_now());
}

/* The caller gives up before the recv and the READY of the skipped recv is
 * lost: the guard armed with it moves the FSM on. */
ZTEST(hio_lte_txn_abort, test_receive_skip_guard)
{
	uint8_t rx[64];
	size_t len = 0;
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		.timeout = K_MSEC(100),
	};

	fsm_to_ready_connected();
	fake_flow_recv_len = 10;
	m_hold_state = "receive";
	fake_rtc_hook = hold_in_state;

	zassert_equal(hio_lte_send_recv(&param), -ETIMEDOUT);
	fill_event_queue();

	k_sleep(K_MSEC(2 * IO_BLOCK_MS));
	zassert_true(fsm_in("receive"), "the READY was not lost: %s", state_now());
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("ready"), "stuck in %s", state_now());
}

/* A recv longer than the guard and a slow coneval, nothing lost: no guard cuts
 * the receive state short, the one armed after the recv runs from its end, and
 * the FSM goes on through the coneval state. */
ZTEST(hio_lte_txn_abort, test_receive_slow_nothing_lost)
{
	uint8_t rx[64];
	size_t len = 0;
	struct hio_lte_send_recv_param param = {
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		.timeout = K_SECONDS(20),
	};

	fsm_to_ready_connected();
	fake_flow_recv_len = 10;
	fake_flow_recv_block_ms = 12000;
	fake_flow_coneval_ms = 7000;

	zassert_ok(hio_lte_send_recv(&param));
	zassert_equal(len, 10);
	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("coneval"), "the receive state was cut short: %s", state_now());
	/* Past the first timer of the ready state too. */
	k_sleep(K_SECONDS(7));
	zassert_true(fsm_in("ready"), "in %s", state_now());

	fake_flow_recv_block_ms = 0;
	fake_flow_coneval_ms = 0;
	len = 0;
	zassert_ok(hio_lte_send_recv(&param));
	zassert_equal(len, 10);
}

/* The modem deregisters early in a recv longer than the guard: no TIMEOUT of
 * the receive state waits behind the DEREGISTERED, so the attach that follows
 * keeps its own timer and is not counted as failed. */
ZTEST(hio_lte_txn_abort, test_receive_dereg_during_slow_recv)
{
	static uint8_t rx[64];
	static size_t len;
	struct hio_lte_metrics before;
	struct hio_lte_metrics after;

	fsm_to_ready_connected();
	zassert_ok(hio_lte_get_metrics(&before));
	fake_flow_recv_len = 10;
	fake_flow_recv_block_ms = 8000;
	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		.timeout = K_SECONDS(20),
	};
	caller_start(K_PRIO_PREEMPT(0));

	k_sleep(K_SECONDS(2));
	zassert_true(fsm_in("receive"), "in %s", state_now());
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_DEREGISTERED);
	caller_join();
	zassert_ok(m_caller_ret, "got %d", m_caller_ret);

	k_sleep(K_SECONDS(1));
	zassert_true(fsm_in("attach"), "the attach was ended: %s", state_now());
	zassert_ok(hio_lte_get_metrics(&after));
	zassert_equal(after.attach_fail_count, before.attach_fail_count,
		      "the attach was counted as failed");
}

static void post_cscon_1(void)
{
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
}

/* CSCON 1 arrives as the send watchdog fires and the FSM thread runs late: the
 * TIMEOUT of the watchdog, queued behind it, reaches the receive state, which
 * ignores it as not due and goes on through the coneval state. */
ZTEST(hio_lte_txn_abort, test_receive_stale_send_timeout)
{
	static uint8_t rx[64];
	static size_t len;

	fsm_bring_up();
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"), "in %s", state_now());
	fake_flow_recv_len = 10;
	m_caller_param = (struct hio_lte_send_recv_param){
		.send_buf = m_payload,
		.send_len = sizeof(m_payload),
		.recv_buf = rx,
		.recv_size = sizeof(rx),
		.recv_len = &len,
		/* SNDTIMEO of 120 s, the watchdog a second later: the caller
		 * does not give up first. */
		.timeout = K_FOREVER,
	};
	caller_start(K_PRIO_PREEMPT(0));

	k_sleep(K_MSEC(100));
	zassert_true(fsm_in("send"), "in %s", state_now());
	fake_flow_coneval_ms = 2000;
	post_before_timer_expiry("send", post_cscon_1);

	caller_join();
	zassert_ok(m_caller_ret, "got %d", m_caller_ret);
	zassert_equal(len, 10);
	k_sleep(K_MSEC(500));
	zassert_true(fsm_in("coneval"), "the receive state took the TIMEOUT of the watchdog: %s",
		     state_now());
}

/* The READY that ends the coneval state is lost: its guard moves the FSM on. */
ZTEST(hio_lte_txn_abort, test_coneval_guard)
{
	fake_flow_coneval_ms = IO_BLOCK_MS;
	fsm_bring_up();

	k_sleep(K_MSEC(100));
	zassert_true(fsm_in("coneval"));
	fill_event_queue();

	k_sleep(K_MSEC(IO_BLOCK_MS));
	zassert_true(fsm_in("coneval"), "the READY was not lost: %s", state_now());
	k_sleep(K_SECONDS(6));
	zassert_true(fsm_in("ready"), "stuck in %s", state_now());
}

/* The SOCKET_OPENED of a socket opened is lost: the guard stands in for it. */
ZTEST(hio_lte_txn_abort, test_open_socket_guard)
{
	fake_flow_open_ms = IO_BLOCK_MS;
	fsm_to_attach();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);

	k_sleep(K_MSEC(100));
	zassert_true(fsm_in("open_socket"));
	fill_event_queue();

	zassert_equal(hio_lte_wait_for_connected(K_MSEC(2 * IO_BLOCK_MS)), -ETIMEDOUT,
		      "the SOCKET_OPENED was not lost");
	zassert_ok(hio_lte_wait_for_connected(K_SECONDS(6)), "stuck in %s", state_now());

	/* No socket opened (the loopback address skips the open), no guard: a
	 * stale TIMEOUT, well after any earlier timer was due, is not taken for
	 * SOCKET_OPENED. */
	static const struct hio_lte_socket_config loopback = {.port = 5002, .addr = "127.0.0.1"};

	fsm_down();
	zassert_ok(hio_lte_enable(&loopback));
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	k_sleep(K_SECONDS(11));
	zassert_true(fsm_in("open_socket"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_TIMEOUT);
	zassert_equal(hio_lte_wait_for_connected(K_MSEC(200)), -ETIMEDOUT,
		      "a stale TIMEOUT taken for SOCKET_OPENED");
}
