/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Link-level fakes for hio_lte.c's dependencies. Flow calls succeed; modem
 * events are injected by the test via fake_flow_event_cb. */

#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_talk.h"

#include <hio/hio_lte.h>
#include <hio/hio_rtc.h>

#include <zephyr/kernel.h>

#include <stdint.h>

struct hio_lte_config g_hio_lte_config;

/* Observed by the tests: how many datagrams the FSM actually handed to the
 * modem, and the parameter block it used for the most recent one. */
atomic_t fake_flow_send_count = ATOMIC_INIT(0);
const struct hio_lte_send_recv_param *fake_flow_last_send_param;

/* Stands in for the time nrf_send() spends waiting for RRC. Holding the FSM
 * inside the send is what lets a caller's deadline expire while its param is
 * still published, which is the condition the transfer needs to go wrong. */
uint32_t fake_flow_send_block_ms;

/* Counts FSM entries into PREPARE (modem power-up). */
atomic_t fake_flow_start_count = ATOMIC_INIT(0);

HIO_LTE_FSM_EVENT_delegate_cb fake_flow_event_cb;

int hio_lte_config_init(void)
{
	return 0;
}

int hio_lte_flow_init(HIO_LTE_FSM_EVENT_delegate_cb cb)
{
	fake_flow_event_cb = cb;
	return 0;
}

int hio_lte_flow_send(const struct hio_lte_send_recv_param *param)
{
	fake_flow_last_send_param = param;
	atomic_inc(&fake_flow_send_count);

	if (fake_flow_send_block_ms != 0) {
		k_sleep(K_MSEC(fake_flow_send_block_ms));
	}

	return 0;
}

int hio_lte_flow_recv(const struct hio_lte_send_recv_param *param)
{
	ARG_UNUSED(param);
	return -ETIMEDOUT;
}

int hio_lte_flow_start(void)
{
	atomic_inc(&fake_flow_start_count);
	return 0;
}

int hio_lte_flow_stop(void)
{
	return 0;
}

int hio_lte_flow_prepare(void)
{
	return 0;
}

int hio_lte_flow_cfun(int cfun)
{
	ARG_UNUSED(cfun);
	return 0;
}

int hio_lte_flow_sim_info(void)
{
	return 0;
}

int hio_lte_flow_sim_fplmn(void)
{
	return 0;
}

int hio_lte_flow_open_socket(const struct hio_lte_socket_config *socket_config,
			     bool load_dtls_session)
{
	ARG_UNUSED(socket_config);
	ARG_UNUSED(load_dtls_session);
	return 0;
}

int hio_lte_flow_close_socket(bool save_dtls_session)
{
	ARG_UNUSED(save_dtls_session);
	return 0;
}

int hio_lte_flow_check(void)
{
	return 0;
}

int hio_lte_flow_set_sndtimeo(int timeout_sec)
{
	ARG_UNUSED(timeout_sec);
	return 0;
}

int hio_lte_flow_coneval(void)
{
	return 0;
}

int hio_lte_flow_cmd(const char *cmd)
{
	ARG_UNUSED(cmd);
	return 0;
}

int hio_lte_flow_set_psk(const char *identity, const char *psk_hex)
{
	ARG_UNUSED(identity);
	ARG_UNUSED(psk_hex);
	return 0;
}

/* Long enough for the test to inject REGISTERED before attach times out. */
#define FAKE_ATTACH_TIMEOUT K_SECONDS(60)

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_periodic(int attempt, k_timeout_t pause)
{
	ARG_UNUSED(attempt);
	return (struct hio_lte_attach_timeout){.attach_timeout = FAKE_ATTACH_TIMEOUT,
					       .retry_delay = pause};
}

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_progressive(int attempt)
{
	ARG_UNUSED(attempt);
	return (struct hio_lte_attach_timeout){.attach_timeout = FAKE_ATTACH_TIMEOUT,
					       .retry_delay = K_SECONDS(1)};
}

int hio_lte_talk_ncellmeas(int p1, int p2)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	return 0;
}

int hio_rtc_get_ts(int64_t *ts)
{
	if (ts != NULL) {
		*ts = 0;
	}
	return 0;
}
