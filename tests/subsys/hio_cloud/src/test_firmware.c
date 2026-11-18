/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* The confirmation of the running image and the ack of a firmware update done by the
 * init work, in the scenario TEST_FW (see mock_fw_setup()). */

#include "fake_backend.h"
#include "hio_cloud_msg.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Standard includes */
#include <errno.h>
#include <string.h>

ZTEST_SUITE(hio_cloud_firmware, phase_started, NULL, NULL, NULL, NULL);

#if TEST_CONFIG_PACK_FAIL
/* A local error of the config comes again on every attempt: the init goes on without it */
ZTEST(hio_cloud_firmware, test_init_without_config)
{
	zassert_true(fake_log_find(UL_CREATE_SESSION) >= 0);
	zassert_equal(fake_log_find(UL_UPLOAD_CONFIG), -1);
}
#endif

ZTEST(hio_cloud_firmware, test_firmware_update_ack)
{
	int session = fake_log_find(UL_CREATE_SESSION);
	int ack = fake_log_find(UL_UPLOAD_FIRMWARE);

	zassert_true(session >= 0);

#if TEST_FW == 0
	/* No update id stored (an update by AT$FW, or the id got lost): the image is
	 * confirmed, but there is nothing to ack (no ack with an uninitialized id) */
	zassert_equal(ack, -1, "ack at %d", ack);
	zassert_equal(mock_boot_confirm_calls(), 1);
	zassert_false(mock_fw_update_id_stored());
#elif TEST_FW == 3
	/* An update id stored, but the image was confirmed before (MCUboot reverted the
	 * update): no ack, and the id is dropped */
	zassert_equal(ack, -1, "ack at %d", ack);
	zassert_equal(mock_boot_confirm_calls(), 0);
	zassert_false(mock_fw_update_id_stored());
#elif TEST_FW == 4
	/* The image cannot be confirmed, so MCUboot reverts it at the next reset: no ack, but
	 * the init goes on, and the id stays for the old image to drop */
	zassert_equal(ack, -1, "ack at %d", ack);
	zassert_equal(mock_boot_confirm_calls(), 1);
	zassert_true(mock_fw_update_id_stored());
#else
	/* Confirmed and acked right after the session */
	zassert_equal(ack, session + 1, "ack at %d, session at %d", ack, session);
	zassert_equal(mock_boot_confirm_calls(), 1);

	const struct fake_log_entry *e = fake_log(ack);

	zassert_equal(e->len, 1 + sizeof(TEST_FW_UPDATE_ID));
	zassert_mem_equal(&e->head[1], TEST_FW_UPDATE_ID, sizeof(TEST_FW_UPDATE_ID));

#if TEST_FW == 2
	/* The first ack fails and goes again 10 s later, in the same boot: that the image is
	 * confirmed by then must not drop the id */
	zassert_equal(e->ret, -EIO);

	const struct fake_log_entry *again = fake_log(ack + 1);

	zassert_not_null(again);
	zassert_equal(again->head[0], UL_UPLOAD_FIRMWARE, "uplink after the failed ack: 0x%02x",
		      again->head[0]);
	zassert_mem_equal(&again->head[1], TEST_FW_UPDATE_ID, sizeof(TEST_FW_UPDATE_ID));
	zassert_within(again->at - e->at, 10000, 100, "ack again %lld ms after the failed one",
		       again->at - e->at);

	e = again;
#endif

	zassert_equal(e->ret, 0);
	/* Acked: the id is dropped */
	zassert_false(mock_fw_update_id_stored());
#endif
}
