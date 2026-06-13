/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/crc.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Test layout
 * ===========
 *
 * The hio_atci subsystem is built by the west module (CONFIG_HIO_ATCI=y). This
 * file provides:
 *   - a mock transport backend (RX ring + TX capture buffer),
 *   - one ATCI instance wired to that backend,
 *   - a set of test-only commands registered with HIO_ATCI_CMD_REGISTER,
 *   - the ztest suite.
 *
 * The ATCI core runs its own thread; tests feed bytes into the RX ring and
 * poll the TX capture buffer until the expected substring/bytes appear.
 */

/* -------------------------------------------------------------------------- */
/* Mock backend                                                               */
/* -------------------------------------------------------------------------- */

#define MOCK_RX_RING_SIZE 1024
#define MOCK_TX_CAP_SIZE   4096

static struct {
	/* RX ring filled by the test, drained by the ATCI thread via read(). */
	uint8_t rx_buf[MOCK_RX_RING_SIZE];
	volatile size_t rx_head; /* write index (test) */
	volatile size_t rx_tail; /* read index (ATCI thread) */
	struct k_spinlock rx_lock;

	/* TX capture: everything the subsystem writes is accumulated here. */
	char tx_buf[MOCK_TX_CAP_SIZE];
	volatile size_t tx_len;
	struct k_spinlock tx_lock;

	hio_atci_backend_handler_t handler;
	void *handler_ctx;
} m_mock;

static int mock_init(const struct hio_atci_backend *backend, const void *config,
		     hio_atci_backend_handler_t handler, void *ctx)
{
	ARG_UNUSED(backend);
	ARG_UNUSED(config);

	m_mock.handler = handler;
	m_mock.handler_ctx = ctx;

	return 0;
}

static int mock_enable(const struct hio_atci_backend *backend)
{
	ARG_UNUSED(backend);
	return 0;
}

static int mock_disable(const struct hio_atci_backend *backend)
{
	ARG_UNUSED(backend);
	return 0;
}

static int mock_write(const struct hio_atci_backend *backend, const void *data, size_t length,
		      size_t *cnt)
{
	ARG_UNUSED(backend);

	k_spinlock_key_t key = k_spin_lock(&m_mock.tx_lock);

	size_t space = MOCK_TX_CAP_SIZE - m_mock.tx_len;
	size_t n = MIN(space, length);

	if (n) {
		memcpy(&m_mock.tx_buf[m_mock.tx_len], data, n);
		m_mock.tx_len += n;
	}

	k_spin_unlock(&m_mock.tx_lock, key);

	if (cnt) {
		/* Report the full length as accepted so the core does not block on
		 * a TXDONE event (the mock has effectively unlimited capacity for
		 * the test payloads). */
		*cnt = length;
	}

	return 0;
}

static int mock_read(const struct hio_atci_backend *backend, void *data, size_t length,
		     size_t *cnt)
{
	ARG_UNUSED(backend);

	k_spinlock_key_t key = k_spin_lock(&m_mock.rx_lock);

	size_t n = 0;
	uint8_t *out = data;

	while (n < length && m_mock.rx_tail != m_mock.rx_head) {
		out[n++] = m_mock.rx_buf[m_mock.rx_tail];
		m_mock.rx_tail = (m_mock.rx_tail + 1) % MOCK_RX_RING_SIZE;
	}

	k_spin_unlock(&m_mock.rx_lock, key);

	if (cnt) {
		*cnt = n;
	}

	return 0;
}

static const struct hio_atci_backend_api mock_backend_api = {
	.init = mock_init,
	.enable = mock_enable,
	.disable = mock_disable,
	.write = mock_write,
	.read = mock_read,
	.update = NULL,
};

static const struct hio_atci_backend mock_backend = {
	.api = &mock_backend_api,
	.ctx = NULL,
};

/* Single ATCI instance under test. The symbol name "test_atci" is also the
 * instance name (STRINGIFY(_name)) used by hio_atci_get_by_name. */
HIO_ATCI_DEFINE(test_atci, &mock_backend, 256, 100);

/* -------------------------------------------------------------------------- */
/* Mock helpers                                                               */
/* -------------------------------------------------------------------------- */

static void mock_tx_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&m_mock.tx_lock);
	m_mock.tx_len = 0;
	m_mock.tx_buf[0] = '\0';
	k_spin_unlock(&m_mock.tx_lock, key);
}

static void mock_feed(const char *s)
{
	size_t len = strlen(s);

	k_spinlock_key_t key = k_spin_lock(&m_mock.rx_lock);
	for (size_t i = 0; i < len; i++) {
		size_t next = (m_mock.rx_head + 1) % MOCK_RX_RING_SIZE;
		if (next == m_mock.rx_tail) {
			/* Ring full; should not happen for test payloads. */
			break;
		}
		m_mock.rx_buf[m_mock.rx_head] = (uint8_t)s[i];
		m_mock.rx_head = next;
	}
	k_spin_unlock(&m_mock.rx_lock, key);

	if (m_mock.handler) {
		m_mock.handler(HIO_ATCI_BACKEND_EVT_RX_RDY, m_mock.handler_ctx);
	}
}

/* Copy the current TX capture into a NUL-terminated caller buffer. */
static size_t mock_tx_snapshot(char *dst, size_t dst_size)
{
	k_spinlock_key_t key = k_spin_lock(&m_mock.tx_lock);
	size_t n = MIN(m_mock.tx_len, dst_size - 1);
	memcpy(dst, m_mock.tx_buf, n);
	dst[n] = '\0';
	k_spin_unlock(&m_mock.tx_lock, key);
	return n;
}

/* Poll the capture buffer until `needle` appears or `timeout_ms` elapses. */
static bool mock_tx_wait_for(const char *needle, int timeout_ms)
{
	static char snap[MOCK_TX_CAP_SIZE];
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (true) {
		mock_tx_snapshot(snap, sizeof(snap));
		if (strstr(snap, needle) != NULL) {
			return true;
		}
		if (k_uptime_get() >= deadline) {
			/* One last check after the deadline. */
			mock_tx_snapshot(snap, sizeof(snap));
			return strstr(snap, needle) != NULL;
		}
		k_msleep(2);
	}
}

#define WAIT_DEFAULT 1000

/* -------------------------------------------------------------------------- */
/* Test-only commands                                                         */
/* -------------------------------------------------------------------------- */

/* +ERRC: returns whatever error code the test stored. Used to check the exact
 * bytes of the error-line mapping. */
static int m_errc_ret;

static int errc_action(const struct hio_atci *atci)
{
	ARG_UNUSED(atci);
	return m_errc_ret;
}
HIO_ATCI_CMD_REGISTER(test_errc, "+ERRC", HIO_ATCI_CMD_ACL_FLAGS_NONE, errc_action, NULL, NULL,
		      NULL, "error code probe");

/* +T: a command exercising all four handler slots. */
static volatile int m_t_action_calls;
static volatile int m_t_read_calls;
static volatile int m_t_test_calls;
static char m_t_set_arg[64];

static int t_action(const struct hio_atci *atci)
{
	m_t_action_calls++;
	hio_atci_printfln(atci, "+T: action");
	return 0;
}

static int t_set(const struct hio_atci *atci, char *argv)
{
	strncpy(m_t_set_arg, argv ? argv : "", sizeof(m_t_set_arg) - 1);
	m_t_set_arg[sizeof(m_t_set_arg) - 1] = '\0';
	hio_atci_printfln(atci, "+T: set %s", m_t_set_arg);
	return 0;
}

static int t_read(const struct hio_atci *atci)
{
	m_t_read_calls++;
	hio_atci_printfln(atci, "+T: read");
	return 0;
}

static int t_test(const struct hio_atci *atci)
{
	m_t_test_calls++;
	hio_atci_printfln(atci, "+T: test");
	return 0;
}
HIO_ATCI_CMD_REGISTER(test_t, "+T", HIO_ATCI_CMD_ACL_FLAGS_NONE, t_action, t_set, t_read, t_test,
		      "all handlers");

/* $X: a command that emits a deferred URC, then a partial line via two printf
 * calls, finished by println. */
static int x_action(const struct hio_atci *atci)
{
	hio_atci_broadcast("@DEFERRED");
	hio_atci_printf(atci, "$X: ");
	hio_atci_printf(atci, "partial");
	hio_atci_printfln(atci, "-line");
	return 0;
}
HIO_ATCI_CMD_REGISTER(test_x, "$X", HIO_ATCI_CMD_ACL_FLAGS_NONE, x_action, NULL, NULL, NULL,
		      "deferred urc");

/* $SLOW: a command that sleeps mid-output between two printf calls. The ATCI
 * thread holds the instance mutex for the whole duration, so a broadcast from
 * another thread cannot interleave. */
static int slow_action(const struct hio_atci *atci)
{
	hio_atci_printf(atci, "$SLOW: ");
	k_msleep(100);
	hio_atci_printfln(atci, "done");
	return 0;
}
HIO_ATCI_CMD_REGISTER(test_slow, "$SLOW", HIO_ATCI_CMD_ACL_FLAGS_NONE, slow_action, NULL, NULL,
		      NULL, "slow handler");

/* +SEC: a command gated by non-zero auth flags. */
static int sec_action(const struct hio_atci *atci)
{
	hio_atci_printfln(atci, "+SEC: ok");
	return 0;
}
HIO_ATCI_CMD_REGISTER(test_sec, "+SEC", 1, sec_action, NULL, NULL, NULL, "auth-gated probe");

/* Auth callback mirroring hio_atci_login: gate flagged commands on the
 * per-instance session state. */
static int auth_check_like_login(const struct hio_atci *atci, const struct hio_atci_cmd *cmd,
				 enum hio_atci_cmd_type type, void *user_data)
{
	ARG_UNUSED(type);
	ARG_UNUSED(user_data);

	if (!cmd || cmd->auth_flags == 0) {
		return 0;
	}

	return hio_atci_auth_get(atci) ? 0 : -EACCES;
}

/* -------------------------------------------------------------------------- */
/* CRC helper                                                                 */
/* -------------------------------------------------------------------------- */

/* Build "<cmd>\t<8-hex-CRC>". crc32_ieee_update(0, cmd, len) is the STANDARD
 * CRC-32 (zlib-compatible): the Zephyr implementation applies the 0xFFFFFFFF
 * init and final XOR internally, so host tooling can use plain zlib crc32. */
static void build_crc_cmd(char *dst, size_t dst_size, const char *cmd)
{
	uint32_t crc = crc32_ieee_update(0, (const uint8_t *)cmd, strlen(cmd));
	snprintf(dst, dst_size, "%s\t%08X", cmd, crc);
}

/* -------------------------------------------------------------------------- */
/* Suite setup                                                                */
/* -------------------------------------------------------------------------- */

static void *suite_setup(void)
{
	int ret = hio_atci_init(&test_atci, NULL, false, 0);
	zassert_ok(ret, "hio_atci_init failed: %d", ret);

	/* Wait for the thread to reach the ACTIVE state by checking that a
	 * trivial command is answered. */
	mock_tx_reset();
	mock_feed("AT\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT),
		     "ATCI instance did not become active");

	return NULL;
}

static void test_before(void *fixture)
{
	ARG_UNUSED(fixture);
	mock_tx_reset();

	/* Defensively reset CRC mode so a failed restore in a CRC test cannot
	 * corrupt subsequent tests. Default build is CRC disabled. */
	test_atci.ctx->crc_mode = 0;
	test_atci.ctx->crc = 0;

	m_errc_ret = 0;
	m_t_action_calls = 0;
	m_t_read_calls = 0;
	m_t_test_calls = 0;
	m_t_set_arg[0] = '\0';
}

ZTEST_SUITE(hio_atci, NULL, suite_setup, test_before, NULL, NULL);

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

/* 1. "AT\n" -> "OK\r\n". */
ZTEST(hio_atci, test_at_ok)
{
	mock_feed("AT\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "no OK response");

	char snap[256];
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "OK\r\n", "unexpected response: '%s'", snap);
}

/* 2. Unknown command vs malformed command. */
ZTEST(hio_atci, test_command_not_found_and_invalid)
{
	mock_feed("ATXX\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Command not found\"\r\n", WAIT_DEFAULT),
		     "expected Command not found");

	mock_tx_reset();

	/* Does not start with "AT" -> -ENOMSG -> "Invalid command". */
	mock_feed("XX\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Invalid command\"\r\n", WAIT_DEFAULT),
		     "expected Invalid command");
}

/* 3. Exact-byte error-code mapping regression (-EIO, -ENOMEM). */
ZTEST(hio_atci, test_error_code_mapping_exact_bytes)
{
	char snap[256];

	m_errc_ret = -EIO;
	mock_feed("AT+ERRC\n");
	zassert_true(mock_tx_wait_for("ERROR: \"I/O error\"\r\n", WAIT_DEFAULT),
		     "no response for -EIO");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "ERROR: \"I/O error\"\r\n", "EIO line wrong: '%s'", snap);

	mock_tx_reset();

	m_errc_ret = -ENOMEM;
	mock_feed("AT+ERRC\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Out of memory\"\r\n", WAIT_DEFAULT),
		     "no response for -ENOMEM");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "ERROR: \"Out of memory\"\r\n", "ENOMEM line wrong: '%s'", snap);
}

/* 4. Dispatch to action/set/read/test. */
ZTEST(hio_atci, test_dispatch_all_handlers)
{
	char snap[256];

	/* AT+T -> action */
	mock_feed("AT+T\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "action: no OK");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "+T: action\r\nOK\r\n", "action output wrong: '%s'", snap);
	zassert_equal(m_t_action_calls, 1, "action not called once");

	/* AT+T=abc -> set receives "abc" */
	mock_tx_reset();
	mock_feed("AT+T=abc\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "set: no OK");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "+T: set abc\r\nOK\r\n", "set output wrong: '%s'", snap);
	zassert_str_equal(m_t_set_arg, "abc", "set arg wrong: '%s'", m_t_set_arg);

	/* AT+T? -> read */
	mock_tx_reset();
	mock_feed("AT+T?\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "read: no OK");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "+T: read\r\nOK\r\n", "read output wrong: '%s'", snap);
	zassert_equal(m_t_read_calls, 1, "read not called once");

	/* AT+T=? -> test */
	mock_tx_reset();
	mock_feed("AT+T=?\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "test: no OK");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "+T: test\r\nOK\r\n", "test output wrong: '%s'", snap);
	zassert_equal(m_t_test_calls, 1, "test not called once");
}

/* 5. AT$CRC set/read consistency. */
ZTEST(hio_atci, test_crc_set_read_consistency)
{
	char snap[256];

	/* Enable strict CRC. The mode changes inside the set handler, i.e.
	 * before the response is framed, so the "OK" of THIS command already
	 * carries a CRC trailer — wait for the OK substring only. */
	mock_feed("AT$CRC=1\n");
	zassert_true(mock_tx_wait_for("OK", WAIT_DEFAULT), "AT$CRC=1 no OK");

	/* AT$CRC? must report 1 (regression: used to be inverted). The command
	 * now needs a valid CRC trailer because strict mode is active. */
	mock_tx_reset();
	{
		char cmd[64];
		build_crc_cmd(cmd, sizeof(cmd), "AT$CRC?");
		strcat(cmd, "\n");
		mock_feed(cmd);
	}
	/* In strict mode the CRC trailer "\t%08X\r\n" is appended to the FINAL
	 * line of the response (OK) only; intermediate lines printed by the
	 * handler end with plain CRLF and the CRC accumulates across the whole
	 * response. */
	zassert_true(mock_tx_wait_for("OK\t", WAIT_DEFAULT), "expected OK with CRC trailer");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_not_null(strstr(snap, "$CRC: 1\r\n"), "missing '$CRC: 1' line: '%s'", snap);

	/* Disable CRC again (valid CRC required because strict mode active). */
	mock_tx_reset();
	{
		char cmd[64];
		build_crc_cmd(cmd, sizeof(cmd), "AT$CRC=0");
		strcat(cmd, "\n");
		mock_feed(cmd);
	}
	zassert_true(mock_tx_wait_for("OK", WAIT_DEFAULT), "AT$CRC=0 no OK");

	/* Now in plain mode: AT$CRC? -> "$CRC: 0\r\nOK\r\n" exactly. */
	mock_tx_reset();
	mock_feed("AT$CRC?\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "AT$CRC? no OK");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "$CRC: 0\r\nOK\r\n", "plain $CRC read wrong: '%s'", snap);
}

/* 6. Strict CRC roundtrip: valid / wrong / missing. */
ZTEST(hio_atci, test_crc_strict_roundtrip)
{
	char cmd[64];
	char snap[256];

	/* Enter strict mode. */
	mock_feed("AT$CRC=1\n");
	zassert_true(mock_tx_wait_for("OK", WAIT_DEFAULT), "AT$CRC=1 no OK");

	/* Valid CRC -> OK with trailer. */
	mock_tx_reset();
	build_crc_cmd(cmd, sizeof(cmd), "AT");
	strcat(cmd, "\n");
	mock_feed(cmd);
	zassert_true(mock_tx_wait_for("OK\t", WAIT_DEFAULT), "valid CRC: no OK with trailer");

	/* Wrong CRC -> CRC mismatch. */
	mock_tx_reset();
	mock_feed("AT\t00000000\n");
	zassert_true(mock_tx_wait_for("ERROR: \"CRC mismatch\"", WAIT_DEFAULT),
		     "wrong CRC: expected mismatch");

	/* Missing CRC (too short / no tab) -> Invalid CRC format. */
	mock_tx_reset();
	mock_feed("AT+T\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Invalid CRC format\"", WAIT_DEFAULT),
		     "missing CRC: expected format error");

	/* Restore disabled mode (valid CRC still required). */
	mock_tx_reset();
	build_crc_cmd(cmd, sizeof(cmd), "AT$CRC=0");
	strcat(cmd, "\n");
	mock_feed(cmd);
	zassert_true(mock_tx_wait_for("OK", WAIT_DEFAULT), "restore AT$CRC=0 no OK");

	/* Confirm plain framing is back. */
	mock_tx_reset();
	mock_feed("AT\n");
	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "plain framing not restored");
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "OK\r\n", "plain framing wrong: '%s'", snap);
}

/* 7. Cross-thread URC with no command in flight. */
ZTEST(hio_atci, test_urc_cross_thread)
{
	int ret = hio_atci_broadcast("@TEST");
	zassert_ok(ret, "broadcast failed: %d", ret);

	zassert_true(mock_tx_wait_for("@TEST\r\n", WAIT_DEFAULT), "URC not emitted");

	char snap[256];
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "@TEST\r\n", "URC line wrong: '%s'", snap);
}

/* 8. Deferred URC: broadcast issued from inside a handler is flushed AFTER the
 *    complete response. */
ZTEST(hio_atci, test_urc_deferred_after_response)
{
	mock_feed("AT$X\n");
	zassert_true(mock_tx_wait_for("@DEFERRED\r\n", WAIT_DEFAULT), "deferred URC missing");

	char snap[256];
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "$X: partial-line\r\nOK\r\n@DEFERRED\r\n",
			  "deferred ordering wrong: '%s'", snap);
}

/* 9. URC vs long handler from another thread: the response line stays
 *    contiguous and the URC only appears after "OK\r\n". */
ZTEST(hio_atci, test_urc_vs_long_handler)
{
	mock_feed("AT$SLOW\n");

	/* Wait until the handler has emitted its partial line. That proves the
	 * ATCI thread is inside the handler holding the instance mutex (and now
	 * sleeping), so the upcoming broadcast is guaranteed to block rather than
	 * race ahead of the response. */
	zassert_true(mock_tx_wait_for("$SLOW: ", WAIT_DEFAULT), "handler did not start");

	/* This blocks on the instance mutex until the handler (and response
	 * framing) completes. */
	int ret = hio_atci_broadcast("@URC");
	zassert_ok(ret, "broadcast failed: %d", ret);

	zassert_true(mock_tx_wait_for("@URC\r\n", WAIT_DEFAULT), "URC missing");

	char snap[256];
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "$SLOW: done\r\nOK\r\n@URC\r\n",
			  "URC interleaved into response: '%s'", snap);
}

/* 10. Command buffer overflow discards the long garbage line. */
ZTEST(hio_atci, test_cmd_buffer_overflow)
{
	/* process_ch stores a char while cmd_buff_len < BUFF_SIZE - 1, otherwise
	 * it logs an overflow and clears the buffer (resetting len to 0). The
	 * fill/clear cycle therefore has period BUFF_SIZE: after every
	 * BUFF_SIZE characters the buffer is back to empty. Feeding an exact
	 * multiple of BUFF_SIZE leaves the buffer empty, so the following "\n"
	 * sees len < 2 in process() and emits nothing. The subsequent "AT\n"
	 * then yields exactly one OK. */
	static char garbage[2 * CONFIG_HIO_ATCI_CMD_BUFF_SIZE + 1];
	memset(garbage, 'A', 2 * CONFIG_HIO_ATCI_CMD_BUFF_SIZE);
	garbage[2 * CONFIG_HIO_ATCI_CMD_BUFF_SIZE] = '\0';

	mock_feed(garbage);
	mock_feed("\n");
	mock_feed("AT\n");

	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "no OK after overflow");

	char snap[256];
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "OK\r\n", "overflow produced extra output: '%s'", snap);
}

/* 11. ESC clears the command buffer. */
ZTEST(hio_atci, test_esc_clears_buffer)
{
	mock_feed("ATGARBAGE\x1b");
	mock_feed("AT\n");

	zassert_true(mock_tx_wait_for("OK\r\n", WAIT_DEFAULT), "no OK after ESC");

	char snap[256];
	mock_tx_snapshot(snap, sizeof(snap));
	zassert_str_equal(snap, "OK\r\n", "ESC did not clear buffer: '%s'", snap);
}

/* 12. hio_atci_get_by_name: exact match only, no prefix matching. */
ZTEST(hio_atci, test_get_by_name_exact)
{
	const struct hio_atci *atci = NULL;

	int ret = hio_atci_get_by_name("test_atci", &atci);
	zassert_ok(ret, "exact name lookup failed: %d", ret);
	zassert_not_null(atci, "instance pointer not set");

	/* A strict prefix must NOT match (regression: used to prefix-match). */
	atci = NULL;
	ret = hio_atci_get_by_name("test_atc", &atci);
	zassert_equal(ret, -ENOENT, "prefix lookup should fail, got %d", ret);
}

/* 13. Per-instance auth gating + auto-logout on backend disconnect. */
ZTEST(hio_atci, test_auth_gating_and_disconnect_logout)
{
	/* The default auth callback denies any command with non-zero flags. */
	mock_feed("AT+SEC\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Permission denied\"\r\n", WAIT_DEFAULT),
		     "expected permission denied with default callback");

	/* Login-like callback gating on the per-instance session state. */
	hio_atci_set_auth_check_cb(auth_check_like_login, NULL);

	mock_tx_reset();
	mock_feed("AT+SEC\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Permission denied\"\r\n", WAIT_DEFAULT),
		     "expected permission denied before login");

	/* Login. */
	hio_atci_auth_set(&test_atci, true);
	zassert_true(hio_atci_auth_get(&test_atci), "auth state not set");

	mock_tx_reset();
	mock_feed("AT+SEC\n");
	zassert_true(mock_tx_wait_for("+SEC: ok\r\nOK\r\n", WAIT_DEFAULT),
		     "expected success after login");

	/* Backend disconnect must end the session automatically. */
	m_mock.handler(HIO_ATCI_BACKEND_EVT_DISABLED, m_mock.handler_ctx);
	zassert_false(hio_atci_auth_get(&test_atci), "session not cleared on disconnect");

	mock_tx_reset();
	mock_feed("AT+SEC\n");
	zassert_true(mock_tx_wait_for("ERROR: \"Permission denied\"\r\n", WAIT_DEFAULT),
		     "expected permission denied after disconnect");
}
