/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_util.h"

#include <zephyr/ztest.h>

#include <string.h>

ZTEST_SUITE(hio_cloud_hash, NULL, NULL, NULL, NULL, NULL);

ZTEST(hio_cloud_hash, test_incremental_matches_oneshot)
{
	static const char data[] = "The quick brown fox jumps over the lazy dog";

	uint8_t oneshot[8];
	zassert_ok(hio_cloud_calculate_hash(oneshot, (const uint8_t *)data, strlen(data), NULL,
					    0));

	struct hio_cloud_hash h;
	uint8_t incremental[8];

	zassert_ok(hio_cloud_hash_begin(&h));
	/* Feed in uneven chunks to exercise streaming. */
	zassert_ok(hio_cloud_hash_update(&h, data, 7));
	zassert_ok(hio_cloud_hash_update(&h, data + 7, 1));
	zassert_ok(hio_cloud_hash_update(&h, data + 8, strlen(data) - 8));
	zassert_ok(hio_cloud_hash_finish(&h, incremental));

	zassert_mem_equal(oneshot, incremental, 8);
}

ZTEST(hio_cloud_hash, test_two_buffer_oneshot_matches_incremental)
{
	static const char part1[] = "0123456789abcdef";
	static const char part2[] = "ghijklmnop";

	uint8_t oneshot[8];
	zassert_ok(hio_cloud_calculate_hash(oneshot, (const uint8_t *)part1, strlen(part1),
					    (const uint8_t *)part2, strlen(part2)));

	struct hio_cloud_hash h;
	uint8_t incremental[8];

	zassert_ok(hio_cloud_hash_begin(&h));
	zassert_ok(hio_cloud_hash_update(&h, part1, strlen(part1)));
	zassert_ok(hio_cloud_hash_update(&h, part2, strlen(part2)));
	zassert_ok(hio_cloud_hash_finish(&h, incremental));

	zassert_mem_equal(oneshot, incremental, 8);
}
