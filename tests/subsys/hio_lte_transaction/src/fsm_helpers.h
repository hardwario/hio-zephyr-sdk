/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef FSM_HELPERS_H_
#define FSM_HELPERS_H_

#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_state.h"

#include <hio/hio_lte.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <string.h>

extern HIO_LTE_FSM_EVENT_delegate_cb fake_flow_event_cb;

/* Drive the FSM to READY, playing the modem's URCs. */
static inline void fsm_bring_up(void)
{
	static const struct hio_lte_socket_config cfg = {.port = 5002, .addr = "192.0.2.1"};

	zassert_ok(hio_lte_enable(&cfg));
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
	zassert_ok(hio_lte_wait_for_connected(K_SECONDS(1)), "FSM did not reach READY");
}

static inline bool fsm_in(const char *state)
{
	const char *current;

	return !hio_lte_get_fsm_state(&current) && !strcmp(current, state);
}

/* Registered, with the PSM active time the network granted (-1: none, so the
 * ready state switches the radio off). */
static inline void fsm_set_psm(int active_time)
{
	struct hio_lte_cereg_param param = {
		.valid = true,
		.stat = HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME,
		.act = HIO_LTE_CEREG_PARAM_ACT_LTE,
		.active_time = active_time,
		.periodic_tau_ext = active_time < 0 ? -1 : 3600,
	};

	hio_lte_state_set_cereg_param(&param);
}

/* The modem registers during a scan (+CEREG: 1): the state before the event,
 * as in the flow. */
static inline void fsm_scan_registers(void)
{
	fsm_set_psm(0);
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_REGISTERED);
}

/* hio_lte_schedule_ncellmeas() first after an enable: with
 * CONFIG_HIO_LTE_NCELLMEAS the enable has requested one. */
#define FIRST_NCELLMEAS_RET (IS_ENABLED(CONFIG_HIO_LTE_NCELLMEAS) ? -EALREADY : 0)

/* With CONFIG_HIO_LTE_NCELLMEAS, end the measurement the enable requested: the
 * release of the RRC connection starts it. */
static inline void fsm_boot_ncellmeas(void)
{
	if (!IS_ENABLED(CONFIG_HIO_LTE_NCELLMEAS)) {
		return;
	}

	zassert_equal(hio_lte_wait_for_ncellmeas(K_NO_WAIT), -ETIMEDOUT, "none requested");
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ready"));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	k_sleep(K_MSEC(50));
	zassert_true(fsm_in("ncellmeas"), "the release did not start it");
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	zassert_ok(hio_lte_wait_for_ncellmeas(K_SECONDS(1)), "measurement request not ended");
	/* The callbacks run after the done bit. */
	k_sleep(K_MSEC(50));
}

/* End a measurement request a test left behind: the others expect none. */
static inline void fsm_end_ncellmeas(void)
{
	if (!hio_lte_wait_for_ncellmeas(K_NO_WAIT)) {
		return;
	}

	g_hio_lte_config.test = false;
	zassert_ok(hio_lte_disable());
	zassert_ok(hio_lte_wait_for_disable(K_SECONDS(10)));
	fsm_set_psm(0);
	fsm_bring_up();
	k_sleep(K_MSEC(50));
	(void)hio_lte_schedule_ncellmeas();
	k_sleep(K_MSEC(50));
	fake_flow_event_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	zassert_ok(hio_lte_wait_for_ncellmeas(K_SECONDS(1)), "measurement request not ended");
}

#endif /* FSM_HELPERS_H_ */
