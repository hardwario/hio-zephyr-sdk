#include "hio_lte_state.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static K_MUTEX_DEFINE(m_lock);

static uint64_t m_imei = 0;
static uint64_t m_imsi = 0;
static char m_iccid[22 + 1] = {0};
static char m_fw_version[64] = {0};
static char m_ceer[64 + 1] = {0};
static struct hio_lte_conn_param m_conn_param = {0};
static struct hio_lte_cereg_param m_cereg_param = {0};
static struct hio_lte_rai_param m_rai_param = {0};
static struct hio_lte_ncellmeas_param m_ncellmeas_param = {0};
static struct hio_lte_scan_result m_scan_result = {0};

/* Ring of CEREG changes; m_cereg_head is the next slot to write. */
static struct hio_lte_cereg_event m_cereg_history[CONFIG_HIO_LTE_CEREG_HISTORY];
static size_t m_cereg_head;
static size_t m_cereg_len;
static struct hio_lte_cereg_event m_last_reject;
static bool m_last_reject_valid;
static int m_dtls_ciphersuite_used = 0;

int hio_lte_state_get_imei(uint64_t *imei)
{
	if (!imei) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*imei = m_imei;

	k_mutex_unlock(&m_lock);

	return *imei ? 0 : -ENODATA;
}

void hio_lte_state_set_imei(uint64_t imei)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	m_imei = imei;

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_imsi(uint64_t *imsi)
{
	if (!imsi) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*imsi = m_imsi;

	k_mutex_unlock(&m_lock);

	return *imsi ? 0 : -ENODATA;
}

void hio_lte_state_set_imsi(uint64_t imsi)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	m_imsi = imsi;

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_iccid(char **iccid)
{
	if (!iccid) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*iccid = m_iccid;

	k_mutex_unlock(&m_lock);

	return *iccid[0] != 0 ? 0 : -ENODATA;
}

void hio_lte_state_set_iccid(const char *iccid)
{
	if (!iccid) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	strncpy(m_iccid, iccid, sizeof(m_iccid));

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_modem_fw_version(char **version)
{
	if (!version) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*version = m_fw_version;

	k_mutex_unlock(&m_lock);

	return m_fw_version[0] != 0 ? 0 : -ENODATA;
}

void hio_lte_state_set_modem_fw_version(const char *version)
{
	if (!version) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	strncpy(m_fw_version, version, sizeof(m_fw_version));

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_ceer(char **ceer)
{
	if (!ceer) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*ceer = m_ceer;

	k_mutex_unlock(&m_lock);

	return m_ceer[0] != 0 ? 0 : -ENODATA;
}

void hio_lte_state_set_ceer(const char *ceer)
{
	if (!ceer) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	strncpy(m_ceer, ceer, sizeof(m_ceer) - 1);
	m_ceer[sizeof(m_ceer) - 1] = '\0';

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_conn_param(struct hio_lte_conn_param *param)
{
	if (!param) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(param, &m_conn_param, sizeof(m_conn_param));

	k_mutex_unlock(&m_lock);

	return 0;
}

void hio_lte_state_set_conn_param(const struct hio_lte_conn_param *param)
{
	if (!param) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(&m_conn_param, param, sizeof(m_conn_param));

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_cereg_param(struct hio_lte_cereg_param *param)
{
	if (!param) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(param, &m_cereg_param, sizeof(m_cereg_param));

	k_mutex_unlock(&m_lock);

	return 0;
}

void hio_lte_state_set_cereg_param(const struct hio_lte_cereg_param *param)
{
	if (!param) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(&m_cereg_param, param, sizeof(m_cereg_param));

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_rai_param(struct hio_lte_rai_param *param)
{
	if (!param) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(param, &m_rai_param, sizeof(m_rai_param));

	k_mutex_unlock(&m_lock);

	return 0;
}

void hio_lte_state_set_rai_param(const struct hio_lte_rai_param *param)
{
	if (!param) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(&m_rai_param, param, sizeof(m_rai_param));

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_ncellmeas_param(struct hio_lte_ncellmeas_param *param)
{
	if (!param) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(param, &m_ncellmeas_param, sizeof(m_ncellmeas_param));

	k_mutex_unlock(&m_lock);
	return 0;
}

static bool cereg_event_same(const struct hio_lte_cereg_event *a,
			     const struct hio_lte_cereg_event *b)
{
	return a->stat == b->stat && a->act == b->act && a->tac == b->tac && a->cid == b->cid &&
	       a->reject_cause == b->reject_cause;
}

void hio_lte_state_add_cereg_event(const struct hio_lte_cereg_param *param)
{
	if (!param) {
		return;
	}

	struct hio_lte_cereg_event event = {
		.uptime_s = k_uptime_seconds(),
		.cid = param->cid,
		.tac = strtoul(param->tac, NULL, 16),
		.stat = param->stat,
		.act = param->act,
		/* cause_type 1 is manufacturer specific, not an EMM cause. */
		.reject_cause = param->cause_type == 0 ? param->reject_cause : 0,
	};

	k_mutex_lock(&m_lock, K_FOREVER);

	if (m_rai_param.valid) {
		event.plmn = m_rai_param.plmn;
	}

	if (event.reject_cause) {
		m_last_reject = event;
		m_last_reject_valid = true;
	}

	/* Repeats (e.g. on every TAU) would push out the real changes. */
	size_t newest = (m_cereg_head + ARRAY_SIZE(m_cereg_history) - 1) %
			ARRAY_SIZE(m_cereg_history);
	if (!m_cereg_len || !cereg_event_same(&m_cereg_history[newest], &event)) {
		m_cereg_history[m_cereg_head] = event;
		m_cereg_head = (m_cereg_head + 1) % ARRAY_SIZE(m_cereg_history);
		m_cereg_len = MIN(m_cereg_len + 1, ARRAY_SIZE(m_cereg_history));
	}

	k_mutex_unlock(&m_lock);
}

int hio_lte_state_get_cereg_history(struct hio_lte_cereg_event *events, size_t max,
				    size_t *count)
{
	if (!events || !count) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*count = MIN(max, m_cereg_len);
	for (size_t i = 0; i < *count; i++) {
		size_t idx = (m_cereg_head + ARRAY_SIZE(m_cereg_history) - 1 - i) %
			     ARRAY_SIZE(m_cereg_history);
		events[i] = m_cereg_history[idx];
	}

	k_mutex_unlock(&m_lock);

	return 0;
}

int hio_lte_state_get_last_reject(struct hio_lte_cereg_event *event)
{
	if (!event) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	*event = m_last_reject;
	bool valid = m_last_reject_valid;
	k_mutex_unlock(&m_lock);

	return valid ? 0 : -ENODATA;
}

int hio_lte_state_get_scan_result(struct hio_lte_scan_result *result)
{
	if (!result) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	memcpy(result, &m_scan_result, sizeof(m_scan_result));
	k_mutex_unlock(&m_lock);

	return 0;
}

void hio_lte_state_set_scan_result(const struct hio_lte_scan_result *result)
{
	if (!result) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	memcpy(&m_scan_result, result, sizeof(m_scan_result));
	k_mutex_unlock(&m_lock);
}

void hio_lte_state_set_ncellmeas_param(const struct hio_lte_ncellmeas_param *param)
{
	if (!param) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	memcpy(&m_ncellmeas_param, param, sizeof(m_ncellmeas_param));

	m_ncellmeas_param.act = m_cereg_param.act;

	k_mutex_unlock(&m_lock);
}

void hio_lte_state_set_dtls_ciphersuite_used(const int cipher)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	m_dtls_ciphersuite_used = cipher;

	k_mutex_unlock(&m_lock);
}

int hio_lte_get_dtls_ciphersuite_used(int *cipher)
{
	if (!cipher) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	*cipher = m_dtls_ciphersuite_used;

	k_mutex_unlock(&m_lock);

	return 0;
}
