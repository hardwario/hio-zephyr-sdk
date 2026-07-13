/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_msg.h"
#include "hio_cloud_util.h"
#include "test_module.h"

#include <hio/hio_buf.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>

#include <string.h>

static void *suite_setup(void)
{
	zassert_ok(test_module_register(), "test module registration failed");

	const struct shell *sh = shell_backend_dummy_get_ptr();
	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "dummy shell backend did not become ready");

	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_module_set_defaults();
}

ZTEST_SUITE(hio_cloud_pack_config, NULL, suite_setup, before_each, NULL, NULL);

/* Oracle: reproduce the legacy dummy-shell derived message and compare. */
ZTEST(hio_cloud_pack_config, test_matches_dummy_shell_oracle)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	zassert_ok(shell_execute_cmd(sh, "config show"));

	size_t raw_size;
	const char *raw = shell_backend_dummy_get_output(sh, &raw_size);
	zassert_not_null(raw);

	uint8_t expected_hash[8];
	zassert_ok(hio_cloud_calculate_hash(expected_hash, (const uint8_t *)raw, raw_size, NULL,
					    0));

	/* 16K: tests run alphabetically, so test_large_config's "big" module
	 * (~11 KB of lines) is already registered when this oracle runs. */
	HIO_BUF_DEFINE(buf, 16384);
	zassert_ok(hio_cloud_msg_pack_config(&buf));

	uint8_t *p = hio_buf_get_mem(&buf);
	size_t used = hio_buf_get_used(&buf);

	zassert_true(used > 10);
	zassert_equal(p[0], UL_UPLOAD_CONFIG);
	zassert_mem_equal(&p[1], expected_hash, 8, "wire hash mismatch");
	zassert_equal(p[9], 0x00, "expected UL_CONFIG_HEADER_NOCOMPRESSION");

	/* Decode the CBOR list and compare against the shell output lines. */
	ZCBOR_STATE_D(zs, 1, p + 10, used - 10, 1, 0);
	zassert_true(zcbor_list_start_decode(zs));

	const char *cursor = raw;
	const char *end = raw + raw_size;
	int lines = 0;

	while (cursor < end) {
		const char *eol = strstr(cursor, "\r\n");
		if (eol == NULL) {
			break;
		}
		if (eol == cursor) {
			cursor += 2;
			continue;
		}

		struct zcbor_string tstr;
		zassert_true(zcbor_tstr_decode(zs, &tstr), "fewer CBOR lines than shell lines");
		zassert_equal(tstr.len, (size_t)(eol - cursor));
		zassert_mem_equal(tstr.value, cursor, tstr.len, "line %d differs", lines);

		lines++;
		cursor = eol + 2;
	}

	struct zcbor_string extra;
	zassert_false(zcbor_tstr_decode(zs, &extra), "more CBOR lines than shell lines");
	zassert_true(zcbor_list_end_decode(zs));
	zassert_true(lines > 0);
}

/* The dummy shell would truncate >8K output; the iterator path must not. */
ZTEST(hio_cloud_pack_config, test_large_config)
{
	static char m_big_strings[40][300];
	static struct hio_config_item m_big_items[40];
	static struct hio_config m_big_module = {
		.name = "big",
		.items = m_big_items,
		.nitems = ARRAY_SIZE(m_big_items),
		.interim = m_big_strings,
		.final = m_big_strings,
		.size = sizeof(m_big_strings),
	};
	static char m_names[40][8];
	static bool m_registered;

	for (int i = 0; i < 40; i++) {
		snprintf(m_names[i], sizeof(m_names[i]), "s%02d", i);

		m_big_items[i] = (struct hio_config_item){
			.name = m_names[i],
			.type = HIO_CONFIG_TYPE_STRING,
			.variable = m_big_strings[i],
			.size = sizeof(m_big_strings[i]),
			.help = "big",
			.default_string = "",
		};
	}
	if (!m_registered) {
		zassert_ok(hio_config_register(&m_big_module));
		m_registered = true;
	}

	/* Fill values AFTER registration — item_init() inside
	 * hio_config_register() resets every item to its default.
	 *
	 * Value length is 220, not the naively larger 250: a 250-char value
	 * makes format_line() produce a 267-byte line, which overflows
	 * item_print_value()'s CONFIG_SHELL_CMD_BUFF_SIZE=256 line buffer and
	 * makes the *oracle's own* "config show" shell command fail with
	 * -ENOSPC (independent of hio_cloud_msg_pack_config) — that would
	 * break test_matches_dummy_shell_oracle once this module is
	 * registered. 220 keeps each line under 256 B while the aggregate
	 * (~40 * 232 B) still exceeds the old 8K dummy-shell total-output
	 * limit that this test targets. */
	for (int i = 0; i < 40; i++) {
		memset(m_big_strings[i], 'x', 220);
		m_big_strings[i][220] = '\0';
	}

	/* ~40 * (10 + 220 + 2) B of lines: over the 8K dummy-shell limit. */
	HIO_BUF_DEFINE(buf, 16384);
	zassert_ok(hio_cloud_msg_pack_config(&buf));

	uint8_t *p = hio_buf_get_mem(&buf);
	size_t used = hio_buf_get_used(&buf);

	ZCBOR_STATE_D(zs, 1, p + 10, used - 10, 1, 0);
	zassert_true(zcbor_list_start_decode(zs));

	int lines = 0;
	bool seen_last = false;
	struct zcbor_string tstr;

	while (zcbor_tstr_decode(zs, &tstr)) {
		lines++;
		if (tstr.len > 15 && memcmp(tstr.value, "big config s39 ", 15) == 0) {
			seen_last = true;
		}
	}
	zassert_true(zcbor_list_end_decode(zs));

	zassert_true(lines >= 40, "expected all big-module lines, got %d", lines);
	zassert_true(seen_last, "last big-module item missing (truncation?)");
}
