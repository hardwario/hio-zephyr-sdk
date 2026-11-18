/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* LTE event callbacks (hio_lte_add_callback()). */

#include "fsm_helpers.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static atomic_t m_cb_count;
static enum hio_lte_event m_cb_events[4];
static void *m_cb_user_data;
static atomic_t m_cb_once_count;

static void cb_handler(struct hio_lte_cb *cb, enum hio_lte_event event)
{
	atomic_val_t n = atomic_inc(&m_cb_count);

	if (n < ARRAY_SIZE(m_cb_events)) {
		m_cb_events[n] = event;
	}

	m_cb_user_data = cb->user_data;
}

/* Removes itself on the first event. */
static void cb_once_handler(struct hio_lte_cb *cb, enum hio_lte_event event)
{
	atomic_inc(&m_cb_once_count);
	hio_lte_remove_callback(cb);
}

static void callbacks_before(void *fixture)
{
	ARG_UNUSED(fixture);

	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(2)));
}

ZTEST_SUITE(hio_lte_callbacks, NULL, NULL, callbacks_before, NULL, NULL);

ZTEST(hio_lte_callbacks, test_cscon)
{
	static int user_data;
	static struct hio_lte_cb cb = {.handler = cb_handler, .user_data = &user_data};
	static struct hio_lte_cb once = {.handler = cb_once_handler};
	static struct hio_lte_cb no_handler;

	zassert_equal(hio_lte_add_callback(NULL), -EINVAL);
	zassert_equal(hio_lte_add_callback(&no_handler), -EINVAL);
	zassert_equal(hio_lte_remove_callback(NULL), -EINVAL);

	/* Ahead of cb: removing itself must not stop the others. */
	zassert_ok(hio_lte_add_callback(&once));
	zassert_ok(hio_lte_add_callback(&cb));
	zassert_equal(hio_lte_add_callback(&cb), -EALREADY);

	fsm_bring_up();
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));

	zassert_equal(atomic_get(&m_cb_count), 2);
	zassert_equal(m_cb_events[0], HIO_LTE_EVENT_CSCON_1);
	zassert_equal(m_cb_events[1], HIO_LTE_EVENT_CSCON_0);
	zassert_equal_ptr(m_cb_user_data, &user_data);

	zassert_equal(atomic_get(&m_cb_once_count), 1, "not removed by its handler");
	zassert_equal(hio_lte_remove_callback(&once), -ENOENT);

	zassert_ok(hio_lte_remove_callback(&cb));
	zassert_equal(hio_lte_remove_callback(&cb), -ENOENT);
}
