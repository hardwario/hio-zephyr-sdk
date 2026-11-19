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

#include <hio/hio_config.h>
#include <hio/hio_lte.h>
#include <hio/hio_rtc.h>

#include <zephyr/kernel.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* Non-zero: block for fake_flow_recv_block_ms, then receive this many bytes. */
size_t fake_flow_recv_len;
uint32_t fake_flow_recv_block_ms;

int hio_lte_flow_recv(const struct hio_lte_send_recv_param *param)
{
	if (!fake_flow_recv_len) {
		return -ETIMEDOUT;
	}

	k_sleep(K_MSEC(fake_flow_recv_block_ms));
	*param->recv_len += fake_flow_recv_len;
	return 0;
}

int hio_lte_flow_start(void)
{
	atomic_inc(&fake_flow_start_count);
	return 0;
}

/* The time AT+CFUN=0 and the modem library shutdown take. */
uint32_t fake_flow_stop_ms;

int hio_lte_flow_stop(void)
{
	if (fake_flow_stop_ms) {
		k_sleep(K_MSEC(fake_flow_stop_ms));
	}

	return 0;
}

int hio_lte_flow_prepare(void)
{
	return 0;
}

int fake_flow_last_cfun = -1;

/* Runs inside the command, as the URCs the modem sends before its OK. */
void (*fake_flow_cfun_hook)(int cfun);

int hio_lte_flow_cfun(int cfun)
{
	if (fake_flow_cfun_hook) {
		fake_flow_cfun_hook(cfun);
	}

	fake_flow_last_cfun = cfun;
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

/* Default is long enough for the test to inject REGISTERED in time. */
uint32_t fake_attach_timeout_ms = 60000;
/* Delay after every third attempt, like the periodic policy. */
uint32_t fake_retry_delay_ms;

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_periodic(int attempt, k_timeout_t pause)
{
	ARG_UNUSED(pause);
	return (struct hio_lte_attach_timeout){
		.attach_timeout = K_MSEC(fake_attach_timeout_ms),
		.retry_delay = attempt % 3 == 2 ? K_MSEC(fake_retry_delay_ms) : K_NO_WAIT};
}

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_progressive(int attempt)
{
	return hio_lte_flow_attach_policy_periodic(attempt, K_NO_WAIT);
}

atomic_t fake_flow_scan_cells_count = ATOMIC_INIT(0);
atomic_t fake_flow_scan_plmn_count = ATOMIC_INIT(0);
bool fake_flow_scan_auto;

void hio_lte_flow_scan_begin(enum hio_lte_scan_mode mode, bool auto_triggered)
{
	ARG_UNUSED(mode);
	fake_flow_scan_auto = auto_triggered;
}

void hio_lte_flow_scan_end(void)
{
}

uint8_t fake_flow_scan_cells_act;
atomic_t fake_flow_scan_rat_count = ATOMIC_INIT(0);
bool fake_flow_scan_rat_lte_m;
bool fake_flow_scan_rat_nb_iot;
/* Uptime of the last RAT switch and of the last cell search start. */
int64_t fake_flow_scan_rat_ms;
int64_t fake_flow_scan_cells_ms;

/* Runs inside the RAT switch, as the URCs the modem sends meanwhile. */
void (*fake_flow_scan_rat_hook)(void);

/* The scan steps in order, e.g. "rat:m cells:7 plmn:7 ". */
char fake_flow_scan_trace[128];

static void scan_trace(const char *fmt, int arg)
{
	size_t len = strlen(fake_flow_scan_trace);

	snprintf(fake_flow_scan_trace + len, sizeof(fake_flow_scan_trace) - len, fmt, arg);
}

/* Non-zero: the searches of test mode never end, their waits return it. */
int fake_flow_scan_wait_ret;

int hio_lte_flow_scan_rat(bool lte_m, bool nb_iot)
{
	if (fake_flow_scan_rat_hook) {
		fake_flow_scan_rat_hook();
	}

	scan_trace(lte_m && nb_iot ? "rat:mn " : lte_m ? "rat:m " : "rat:n ", 0);
	fake_flow_scan_rat_lte_m = lte_m;
	fake_flow_scan_rat_nb_iot = nb_iot;
	fake_flow_scan_rat_ms = k_uptime_get();
	atomic_inc(&fake_flow_scan_rat_count);
	return 0;
}

int hio_lte_flow_scan_cells_start(uint8_t act)
{
	scan_trace("cells:%u ", act);
	fake_flow_scan_cells_act = act;
	fake_flow_scan_cells_ms = k_uptime_get();
	atomic_inc(&fake_flow_scan_cells_count);
	return 0;
}

int hio_lte_flow_scan_cells_wait(k_timeout_t timeout)
{
	if (fake_flow_scan_wait_ret) {
		k_sleep(timeout);
	}
	return fake_flow_scan_wait_ret;
}

int64_t fake_flow_scan_plmn_ms;
uint8_t fake_flow_scan_plmn_act;

int hio_lte_flow_scan_plmn_start(uint8_t act)
{
	scan_trace("plmn:%u ", act);
	fake_flow_scan_plmn_act = act;
	fake_flow_scan_plmn_ms = k_uptime_get();
	atomic_inc(&fake_flow_scan_plmn_count);
	return 0;
}

int hio_lte_flow_scan_plmn_wait(k_timeout_t timeout)
{
	if (fake_flow_scan_wait_ret) {
		k_sleep(timeout);
	}
	return fake_flow_scan_wait_ret;
}

atomic_t fake_flow_scan_abort_count = ATOMIC_INIT(0);

void hio_lte_flow_scan_abort(void)
{
	atomic_inc(&fake_flow_scan_abort_count);
}

int hio_lte_flow_abort(void)
{
	return 0;
}

atomic_t fake_flow_ncellmeas_count = ATOMIC_INIT(0);

static nrf_modem_at_resp_handler_t m_ncellmeas_resp_cb;

int hio_lte_talk_ncellmeas_cb(int p1, int p2, nrf_modem_at_resp_handler_t cb)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	m_ncellmeas_resp_cb = cb;
	atomic_inc(&fake_flow_ncellmeas_count);
	return 0;
}

/* The modem's response to the last AT%NCELLMEAS (in ISR context there). */
void fake_flow_ncellmeas_resp(const char *resp)
{
	if (m_ncellmeas_resp_cb) {
		m_ncellmeas_resp_cb(resp);
	}
}

/* The measurement of test mode, without the FSM. */
atomic_t fake_flow_ncellmeas_test_count = ATOMIC_INIT(0);
int fake_flow_ncellmeas_start_ret;
static K_SEM_DEFINE(m_ncellmeas_test_sem, 0, 1);
static const char *m_ncellmeas_test_error;

int hio_lte_flow_ncellmeas_start(void)
{
	k_sem_reset(&m_ncellmeas_test_sem);
	atomic_inc(&fake_flow_ncellmeas_test_count);
	return fake_flow_ncellmeas_start_ret;
}

int hio_lte_flow_ncellmeas_wait(k_timeout_t timeout, char *error, size_t size)
{
	if (k_sem_take(&m_ncellmeas_test_sem, timeout)) {
		return -EAGAIN;
	}

	if (m_ncellmeas_test_error) {
		snprintf(error, size, "%s", m_ncellmeas_test_error);
		return -EIO;
	}

	return 0;
}

/* The URC (stored by the test before) or the refusal of the modem. */
void fake_flow_ncellmeas_done(const char *error)
{
	m_ncellmeas_test_error = error;
	k_sem_give(&m_ncellmeas_test_sem);
}

/* Used by the 'lte' shell commands only. */
int hio_lte_flow_xmodemtrace(int lvl)
{
	return 0;
}

int hio_lte_talk_at_cmd(const char *s)
{
	return 0;
}

int hio_lte_talk_bypass_set_cb(hio_lte_talk_bypass_cb cb, void *user_data)
{
	return 0;
}

int hio_config_shell_cmd(const struct shell *shell, size_t argc, char **argv)
{
	return 0;
}

int hio_rtc_get_ts(int64_t *ts)
{
	if (ts != NULL) {
		*ts = 0;
	}
	return 0;
}
