/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Ownership contract of hio_lte_send_recv().
 *
 * The param passed in is the caller's own storage — hio_cloud_transfer builds it
 * as a per-attempt stack local. hio_lte publishes it in m_send_recv_param so the
 * FSM work queue can act on it, which means the handle must not outlive the
 * call: once hio_lte_send_recv() returns, the FSM must no longer hold a pointer
 * into a frame the caller is about to reuse.
 *
 * These tests recreate the field condition by holding the FSM inside
 * hio_lte_flow_send() (fake_flow_send_block_ms) for longer than the caller's
 * deadline, which is what nrf_send() does while it waits for RRC. */

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Module-internal handle the FSM work queue reads (hio_lte.c). */
extern struct hio_lte_send_recv_param *m_send_recv_param;

extern atomic_t fake_flow_send_count;
extern const struct hio_lte_send_recv_param *fake_flow_last_send_param;
extern uint32_t fake_flow_send_block_ms;

#define UPLINK_LEN 481

/* Comfortably longer than CALLER_DEADLINE_MS, so the caller always gives up
 * first, and long enough that the FSM is still inside the send when it does. */
#define SEND_BLOCK_MS     400
#define CALLER_DEADLINE_MS 100

static uint8_t m_uplink[UPLINK_LEN];

static K_THREAD_STACK_DEFINE(m_holder_stack, 2048);
static struct k_thread m_holder_thread;

static struct hio_lte_send_recv_param m_holder_param;
static int m_holder_ret;

static void reset_fakes(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Let any send left running by the previous test drain, so its
	 * bookkeeping cannot leak into this one. */
	k_sleep(K_MSEC(SEND_BLOCK_MS * 2));

	fake_flow_send_block_ms = 0;
	fake_flow_last_send_param = NULL;
	atomic_clear(&fake_flow_send_count);
}

ZTEST_SUITE(hio_lte_transaction, NULL, NULL, reset_fakes, NULL, NULL);

/* A caller that gives up must take its handle with it. */
ZTEST(hio_lte_transaction, test_timeout_releases_caller_param)
{
	fake_flow_send_block_ms = SEND_BLOCK_MS;

	struct hio_lte_send_recv_param param = {
		.send_buf = m_uplink,
		.send_len = sizeof(m_uplink),
		.timeout = K_MSEC(CALLER_DEADLINE_MS),
	};

	int ret = hio_lte_send_recv(&param);

	zassert_equal(ret, -ETIMEDOUT, "expected the call to end on its own deadline, got %d",
		      ret);
	zassert_is_null(m_send_recv_param,
			"FSM still holds the caller's param after the caller gave up");
}

/* The abandoned frame must never reach the modem. Left published, the handle
 * survives into the FSM's own retry, so a later on_enter_send() transmits from —
 * and validates its result against — whatever now occupies that stack slot. */
ZTEST(hio_lte_transaction, test_abandoned_param_is_never_sent_again)
{
	fake_flow_send_block_ms = SEND_BLOCK_MS;

	struct hio_lte_send_recv_param param = {
		.send_buf = m_uplink,
		.send_len = sizeof(m_uplink),
		.timeout = K_MSEC(CALLER_DEADLINE_MS),
	};

	int ret = hio_lte_send_recv(&param);
	zassert_equal(ret, -ETIMEDOUT, "expected the call to end on its own deadline, got %d",
		      ret);

	int sends_at_giveup = atomic_get(&fake_flow_send_count);

	/* Long enough for the FSM to finish the in-flight send and come back
	 * round through ERROR/READY to another send attempt. */
	k_sleep(K_SECONDS(7));

	zassert_equal(atomic_get(&fake_flow_send_count), sends_at_giveup,
		      "FSM transmitted again after the caller abandoned the transaction");
	zassert_not_equal(fake_flow_last_send_param, &param,
			  "FSM is still using the abandoned caller param");
}

static void holder_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	m_holder_ret = hio_lte_send_recv(&m_holder_param);
}

/* Only one transaction can be in flight, because there is only one handle for
 * the FSM to read. A second caller that cannot take the lock must fail instead
 * of overwriting the handle — otherwise the FSM is pointed at this caller's
 * buffers while the first caller is still blocked waiting for its own. */
ZTEST(hio_lte_transaction, test_concurrent_caller_does_not_steal_transaction)
{
	m_holder_param = (struct hio_lte_send_recv_param){
		.send_buf = m_uplink,
		.send_len = sizeof(m_uplink),
		.timeout = K_SECONDS(2),
	};

	k_thread_create(&m_holder_thread, m_holder_stack, K_THREAD_STACK_SIZEOF(m_holder_stack),
			holder_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);

	/* Let the holder publish its param and block on its own deadline. */
	k_sleep(K_MSEC(200));
	zassert_equal_ptr(m_send_recv_param, &m_holder_param,
			  "holder did not publish its transaction");

	struct hio_lte_send_recv_param intruder = {
		.send_buf = m_uplink,
		.send_len = sizeof(m_uplink),
		.timeout = K_MSEC(CALLER_DEADLINE_MS),
	};

	int ret = hio_lte_send_recv(&intruder);

	zassert_equal(ret, -EBUSY, "second caller should be rejected as busy, got %d", ret);
	zassert_equal_ptr(m_send_recv_param, &m_holder_param,
			  "second caller retargeted the in-flight transaction");

	zassert_ok(k_thread_join(&m_holder_thread, K_SECONDS(5)), "holder did not finish");
	zassert_equal(m_holder_ret, -ETIMEDOUT, "holder should end on its own deadline, got %d",
		      m_holder_ret);
}
