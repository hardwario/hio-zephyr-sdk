/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Mock MCUboot image state and the stored firmware update id (hio_cloud_util.c is not
 * compiled here) for the firmware confirmation of hio_cloud.c (compiled with
 * CONFIG_MCUBOOT_IMG_MANAGER), in the scenario selected by TEST_FW. */

#include "fake_backend.h"
#include "hio_cloud_msg.h"
#include "hio_cloud_util.h"

/* Zephyr includes */
#include <zephyr/dfu/mcuboot.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <string.h>

const uint8_t TEST_FW_UPDATE_ID[16] = {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
				       0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f};

static bool m_confirmed = true;
static int m_confirm_calls;
static bool m_id_stored;

bool boot_is_img_confirmed(void)
{
	return m_confirmed;
}

int boot_write_img_confirmed(void)
{
	m_confirm_calls++;

#if TEST_FW == 4
	/* The flash write fails */
	return -EIO;
#else
	m_confirmed = true;

	return 0;
#endif
}

int hio_cloud_util_get_firmware_update_id(hio_cloud_uuid_t uuid)
{
	if (!m_id_stored) {
		return -ENOENT;
	}

	memcpy(uuid, TEST_FW_UPDATE_ID, sizeof(hio_cloud_uuid_t));

	return 0;
}

int hio_cloud_util_delete_firmware_update_id(void)
{
	m_id_stored = false;

	return 0;
}

int mock_boot_confirm_calls(void)
{
	return m_confirm_calls;
}

bool mock_fw_update_id_stored(void)
{
	return m_id_stored;
}

void mock_fw_setup(void)
{
	/* 3: MCUboot reverted the update, the old image runs confirmed; otherwise the image
	 * has just been swapped in and is not confirmed yet */
	m_confirmed = TEST_FW == 3;
	m_id_stored = TEST_FW >= 1;

#if TEST_FW == 2
	fake_fail_first_of_type(UL_UPLOAD_FIRMWARE, -EIO);
#endif
}
