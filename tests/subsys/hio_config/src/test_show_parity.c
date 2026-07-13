/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "test_module.h"

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <string.h>

/* Calibrated against the real dummy-shell capture in this test: the shell
 * emits a single leading CRLF before the first line. If this test starts
 * failing on the prefix, the cloud hash input rule (hio_cloud_msg_pack_config)
 * must be updated in lockstep — wire-hash compatibility depends on it. */
#define SHOW_STREAM_LEADING_CRLF 1

struct stream_ctx {
	char buf[16384];
	size_t len;
};

static int build_stream_cb(const struct hio_config *module, const struct hio_config_item *item,
			   void *user_data)
{
	struct stream_ctx *ctx = user_data;
	char line[512];

	if (!(hio_config_item_access(module, item) & HIO_CONFIG_ACCESS_SHOW)) {
		return 0;
	}

	int len = hio_config_item_format_line(module, item, line, sizeof(line));
	zassert_true(len > 0);

	zassert_true(ctx->len + len + 2 < sizeof(ctx->buf));
	memcpy(ctx->buf + ctx->len, line, len);
	ctx->len += len;
	memcpy(ctx->buf + ctx->len, "\r\n", 2);
	ctx->len += 2;

	return 0;
}

/* Suites run in alphabetical order; register here too (idempotent). */
static void *suite_setup(void)
{
	zassert_ok(test_module_register(), "test module registration failed");

	/* shell_init() spawns the shell processing thread and returns before it
	 * reaches SHELL_STATE_ACTIVE; shell_execute_cmd() run too early is
	 * silently swallowed by shell_vfprintf() (state != ACTIVE -> no-op), so
	 * the dummy backend would capture nothing. Wait for readiness once here. */
	const struct shell *sh = shell_backend_dummy_get_ptr();
	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "dummy shell backend did not become ready");

	return NULL;
}

ZTEST_SUITE(hio_config_show_parity, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(hio_config_show_parity, test_dummy_shell_show_parity)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	test_module_set_defaults();

	shell_backend_dummy_clear_output(sh);
	zassert_ok(shell_execute_cmd(sh, "config show"));

	size_t raw_size;
	const char *raw = shell_backend_dummy_get_output(sh, &raw_size);
	zassert_not_null(raw);

	struct stream_ctx ctx = {0};
#if SHOW_STREAM_LEADING_CRLF
	memcpy(ctx.buf, "\r\n", 2);
	ctx.len = 2;
#endif
	zassert_ok(hio_config_iter_items(NULL, build_stream_cb, &ctx));

	/* Guard against a vacuously-passing comparison of two empty streams. */
	zassert_true(ctx.len > 2, "generated stream is empty — module not registered?");
	zassert_not_null(strstr(ctx.buf, "app config interval"));

	/* Hidden item must not appear in either stream. */
	zassert_is_null(strstr(raw, "secret"));
	zassert_is_null(strstr(ctx.buf, "secret"));

	zassert_equal(raw_size, ctx.len, "size mismatch: shell %u vs generated %u\n"
		      "shell:\n%.*s\ngenerated:\n%.*s",
		      (unsigned)raw_size, (unsigned)ctx.len, (int)raw_size, raw, (int)ctx.len,
		      ctx.buf);
	zassert_mem_equal(raw, ctx.buf, raw_size, "byte mismatch\nshell:\n%.*s\ngenerated:\n%.*s",
			  (int)raw_size, raw, (int)ctx.len, ctx.buf);
}
