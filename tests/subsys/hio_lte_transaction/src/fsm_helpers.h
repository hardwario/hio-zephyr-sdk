/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef FSM_HELPERS_H_
#define FSM_HELPERS_H_

#include "hio_lte_config.h"
#include "hio_lte_flow.h"

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

#endif /* FSM_HELPERS_H_ */
