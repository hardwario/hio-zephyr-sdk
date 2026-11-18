/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Fake transport behind the hio_cloud backend seam and shared test helpers */

#include "fake_backend.h"

#include "hio_cloud_backend.h"
#include "hio_cloud_msg.h"

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_cloud.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <string.h>

#define HELPER_STACK_SIZE 4096

struct fake g_fake;

const uint8_t g_payload[] = {0xbf, 0x01, 0x18, 0x2a, 0xff};
const uint8_t g_payload2[] = {0xa1, 0x02, 0x03};
uint8_t g_big[HIO_CLOUD_TRANSFER_BUF_SIZE];

static K_SEM_DEFINE(m_gate, 0, 1);
static K_SEM_DEFINE(m_uplink_blocked, 0, 1);
static K_SEM_DEFINE(m_downlink_sem, 0, 1);

/* Set by the CREATE SESSION uplink, answered by the next downlink */
static bool m_session_pending;

bool phase_before_init(const void *state)
{
	return !((const struct test_state *)state)->started;
}

bool phase_started(const void *state)
{
	return ((const struct test_state *)state)->started;
}

void fake_reset(void)
{
	memset(&g_fake, 0, sizeof(g_fake));
	k_sem_reset(&m_gate);
	k_sem_reset(&m_uplink_blocked);
	k_sem_reset(&m_downlink_sem);
}

void fake_before(void *fixture)
{
	ARG_UNUSED(fixture);
	/* Deferred poll work of a previous test must not leak into this one */
	settle();
	fake_reset();
}

void fake_release(void)
{
	k_sem_give(&m_gate);
}

int fake_wait_uplink_blocked(k_timeout_t timeout)
{
	return k_sem_take(&m_uplink_blocked, timeout);
}

int fake_wait_downlink(k_timeout_t timeout)
{
	return k_sem_take(&m_downlink_sem, timeout);
}

static int fake_init(uint32_t serial_number, const uint8_t token[16])
{
	return g_fake.init_ret;
}

static int fake_wait_ready(k_timeout_t timeout)
{
	return 0;
}

static int m_log_count;
static struct fake_log_entry m_log[FAKE_LOG_MAX];
static uint8_t m_fail_type;
static int m_fail_type_err;

int fake_log_count(void)
{
	return m_log_count;
}

const struct fake_log_entry *fake_log(int index)
{
	if (index < 0 || index >= MIN(m_log_count, FAKE_LOG_MAX)) {
		return NULL;
	}

	return &m_log[index];
}

int fake_log_find(uint8_t type)
{
	for (int i = 0; i < MIN(m_log_count, FAKE_LOG_MAX); i++) {
		if (m_log[i].len && m_log[i].head[0] == type) {
			return i;
		}
	}

	return -1;
}

void fake_fail_first_of_type(uint8_t type, int err)
{
	m_fail_type = type;
	m_fail_type_err = err;
}

static int fake_uplink(struct hio_buf *buf, bool *has_downlink, k_timeout_t timeout)
{
	size_t len = hio_buf_get_used(buf);
	uint8_t *mem = hio_buf_get_mem(buf);
	struct fake_log_entry *e = NULL;

	if (m_log_count < FAKE_LOG_MAX) {
		e = &m_log[m_log_count];
		memcpy(e->head, mem, MIN(len, sizeof(e->head)));
		e->len = len;
		e->at = k_uptime_get();
	}

	m_log_count++;

	if (len > 0 && mem[0] == UL_CREATE_SESSION) {
		m_session_pending = true;
		*has_downlink = true;
		return 0;
	}

	if (m_fail_type_err && len > 0 && mem[0] == m_fail_type) {
		int err = m_fail_type_err;

		m_fail_type_err = 0;

		if (e) {
			e->ret = err;
		}

		return err;
	}

	if (g_fake.uplink_count < FAKE_MAX_UPLINKS) {
		struct fake_uplink *up = &g_fake.uplinks[g_fake.uplink_count];

		memcpy(up->head, mem, MIN(len, sizeof(up->head)));
		up->len = len;
		up->crc = crc32_ieee(mem, len);
		up->buf_size = buf->size;
		up->timeout = timeout;
		up->thread = k_current_get();
	}

	g_fake.uplink_count++;

	if (g_fake.uplink_block > 0) {
		g_fake.uplink_block--;
		k_sem_give(&m_uplink_blocked);
		/* Bounded, so that a broken test cannot hang the run */
		k_sem_take(&m_gate, K_SECONDS(2));
	}

	*has_downlink = g_fake.uplink_has_downlink;

	return g_fake.uplink_ret;
}

static int fake_downlink(struct hio_buf *buf, bool *has_downlink, k_timeout_t timeout)
{
	*has_downlink = false;

	if (m_session_pending) {
		m_session_pending = false;
		return hio_buf_append_u8(buf, DL_SET_SESSION);
	}

	/* Distinct bytes, so that a downlink overwriting a message is noticed */
	static const uint8_t dl_bytes[] = {0xde, 0xad, 0xbe, 0xef};

	hio_buf_append_mem(buf, dl_bytes, sizeof(dl_bytes));

	g_fake.downlink_count++;
	g_fake.downlink_thread = k_current_get();
	k_sem_give(&m_downlink_sem);

	return 0;
}

const struct hio_cloud_backend hio_cloud_backend_udp_lte = {
	.init = fake_init,
	.wait_ready = fake_wait_ready,
	.uplink = fake_uplink,
	.downlink = fake_downlink,
};

void settle(void)
{
	k_sleep(K_MSEC(100));
}

void assert_data_msg(int idx, const uint8_t *payload, size_t len)
{
	static const uint8_t hdr[HDR_LEN] = {0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

	zassert_true(idx < g_fake.uplink_count, "uplink %d missing", idx);

	const struct fake_uplink *up = &g_fake.uplinks[idx];

	zassert_equal(up->len, HDR_LEN + len, "uplink %d length %zu", idx, up->len);
	zassert_mem_equal(up->head, hdr, HDR_LEN, "uplink %d header", idx);
	zassert_mem_equal(up->head + HDR_LEN, payload, MIN(len, FAKE_HEAD_LEN - HDR_LEN),
			  "uplink %d payload", idx);
}

void assert_data_msg_crc(int idx, const uint8_t *payload, size_t len)
{
	static const uint8_t hdr[HDR_LEN] = {0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

	zassert_true(idx < g_fake.uplink_count, "uplink %d missing", idx);

	uint32_t crc = crc32_ieee_update(0, hdr, HDR_LEN);

	crc = crc32_ieee_update(crc, payload, len);

	zassert_equal(g_fake.uplinks[idx].len, HDR_LEN + len, "uplink %d length", idx);
	zassert_equal(g_fake.uplinks[idx].crc, crc, "uplink %d message differs", idx);
}

void assert_timeout_in_ms(int idx, int lo_ms, int hi_ms)
{
	zassert_true(idx < g_fake.uplink_count, "uplink %d missing", idx);

	k_ticks_t ticks = g_fake.uplinks[idx].timeout.ticks;

	zassert_true(ticks >= K_MSEC(lo_ms).ticks && ticks <= K_MSEC(hi_ms).ticks,
		     "transfer timeout %lld ticks", (long long)ticks);
}

int send_payload2(void *arg)
{
	return hio_cloud_send_data(g_payload2, sizeof(g_payload2), K_MSEC(10));
}

/* ---- Helper threads ---- */

static K_THREAD_STACK_ARRAY_DEFINE(m_helper_stacks, HELPER_COUNT + 1, HELPER_STACK_SIZE);

struct helper {
	struct k_thread thread;
	int (*fn)(void *arg);
	void *arg;
	int result;
};

/* The last one is the lock probe of assert_lock_free() */
static struct helper m_helpers[HELPER_COUNT + 1];

static void helper_entry(void *p1, void *p2, void *p3)
{
	struct helper *h = p1;

	h->result = h->fn(h->arg);
}

void helper_start(int idx, int (*fn)(void *arg), void *arg)
{
	struct helper *h = &m_helpers[idx];

	h->fn = fn;
	h->arg = arg;
	h->result = INT_MIN;

	k_thread_create(&h->thread, m_helper_stacks[idx], HELPER_STACK_SIZE, helper_entry, h, NULL,
			NULL, k_thread_priority_get(k_current_get()), 0, K_NO_WAIT);
}

int helper_join(int idx, k_timeout_t timeout, int *result)
{
	struct helper *h = &m_helpers[idx];

	if (k_thread_join(&h->thread, timeout)) {
		k_thread_abort(&h->thread);
		return -EAGAIN;
	}

	if (result) {
		*result = h->result;
	}

	return 0;
}

static int probe_lock(void *arg)
{
	/* Takes and releases the cloud lock (m_lock) without touching the transfer buffer */
	return hio_cloud_set_callback(NULL, NULL);
}

void assert_lock_free(void)
{
	int result = INT_MIN;

	helper_start(HELPER_COUNT, probe_lock, NULL);

	zassert_ok(helper_join(HELPER_COUNT, K_MSEC(500), &result), "cloud lock still held");
	zassert_ok(result);
}
