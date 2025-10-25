
/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_atci_io.h"

/* HIO includes */
#include <hio/hio_atci.h>
#include <hio/hio_tok.h>

/* Zephyr includes */
#include "bootutil/bootutil_public.h"
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/storage/flash_map.h>

/* NCS includes */
#include <dfu/dfu_target.h>
#include <dfu/dfu_target_mcuboot.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// jinak pro fw chci
// AT$FW="list"
// AT$FW="start",size
// AT$FW="chunk","base64"
// AT$FW="end" - predstava ze ti rekne ze je vse ok
// AT$FW="swap"

LOG_MODULE_DECLARE(hio_atci_cmd, CONFIG_HIO_ATCI_LOG_LEVEL);

#ifdef CONFIG_TRUSTED_EXECUTION_NONSECURE
#define SLOT0_LABEL slot0_ns_partition
#define SLOT1_LABEL slot1_ns_partition
#else
#define SLOT0_LABEL slot0_partition
#define SLOT1_LABEL slot1_partition
#endif /* CONFIG_TRUSTED_EXECUTION_NONSECURE */

#define FLASH_AREA_IMAGE_PRIMARY FIXED_PARTITION_ID(SLOT0_LABEL)
#if FIXED_PARTITION_EXISTS(SLOT1_LABEL)
#define FLASH_AREA_IMAGE_SECONDARY FIXED_PARTITION_ID(SLOT1_LABEL)
#endif

static const char *swap_type_str(uint8_t type)
{
	switch (type) {
	case BOOT_SWAP_TYPE_NONE:
		return "none";
	case BOOT_SWAP_TYPE_TEST:
		return "test";
	case BOOT_SWAP_TYPE_PERM:
		return "perm";
	case BOOT_SWAP_TYPE_REVERT:
		return "revert";
	case BOOT_SWAP_TYPE_FAIL:
		return "fail";
	}

	return "unknown";
}

static const char *swap_state_magic_str(uint8_t magic)
{
	switch (magic) {
	case BOOT_MAGIC_GOOD:
		return "good";
	case BOOT_MAGIC_BAD:
		return "bad";
	case BOOT_MAGIC_UNSET:
		return "unset";
	case BOOT_MAGIC_ANY:
		return "any";
	case BOOT_MAGIC_NOTGOOD:
		return "notgood";
	}

	return "unknown";
}

static const char *swap_state_flag_str(uint8_t flag)
{
	switch (flag) {
	case BOOT_FLAG_SET:
		return "set";
	case BOOT_FLAG_BAD:
		return "bad";
	case BOOT_FLAG_UNSET:
		return "unset";
	case BOOT_FLAG_ANY:
		return "any";
	}

	return "unknown";
}

static void area_print_info(const struct hio_atci *atci, uint8_t area_id, const char *area_name)
{
	struct mcuboot_img_header hdr;
	struct boot_swap_state swap_state;
	int err;

	hio_atci_printfln(atci, "$FW: \"%s\",\"area\",%u", area_name, area_id);

	err = boot_read_bank_header(area_id, &hdr, sizeof(hdr));
	if (err) {
		hio_atci_printfln(atci, "$FW: \"%s\",\"error\",\"failed to read header: %d\"",
				  area_name, err);
		return;
	}
	err = boot_read_swap_state_by_id(area_id, &swap_state);
	if (err) {
		hio_atci_printfln(atci, "$FW: \"%s\",\"error\",\"failed to read swap state: %d\"",
				  area_name, err);
		return;
	}

	hio_atci_printfln(atci, "$FW: \"%s\",\"version\",\"%u.%u.%u\"", area_name,
			  hdr.h.v1.sem_ver.major, hdr.h.v1.sem_ver.minor,
			  hdr.h.v1.sem_ver.revision);
	hio_atci_printfln(atci, "$FW: \"%s\",\"image size\",%u", area_name, hdr.h.v1.image_size);
	hio_atci_printfln(atci, "$FW: \"%s\",\"magic\",\"%s\"", area_name,
			  swap_state_magic_str(swap_state.magic));
	hio_atci_printfln(atci, "$FW: \"%s\",\"swap type\",\"%s\"", area_name,
			  swap_type_str(swap_state.swap_type));
	hio_atci_printfln(atci, "$FW: \"%s\",\"copy done\",\"%s\"", area_name,
			  swap_state_flag_str(swap_state.copy_done));
	hio_atci_printfln(atci, "$FW: \"%s\",\"image ok\",\"%s\"", area_name,
			  swap_state_flag_str(swap_state.image_ok));
}

static int info(const struct hio_atci *atci)
{
	area_print_info(atci, FLASH_AREA_IMAGE_PRIMARY, "primary");
#if defined(FLASH_AREA_IMAGE_SECONDARY)
	area_print_info(atci, FLASH_AREA_IMAGE_SECONDARY, "secondary");
#else
	hio_atci_println(atci, "$FW: \"secondary\",\"error\",\"secondary area not available\"");
#endif
	return 0;
}

static void dfu_target_callback_handler(enum dfu_target_evt_id evt)
{
	switch (evt) {
	case DFU_TARGET_EVT_ERASE_PENDING:
		LOG_INF("DFU_TARGET_EVT_ERASE_PENDING");
		break;
	case DFU_TARGET_EVT_TIMEOUT:
		LOG_ERR("DFU_TARGET_EVT_TIMEOUT");
		break;
	case DFU_TARGET_EVT_ERASE_DONE:
		LOG_INF("DFU_TARGET_EVT_ERASE_DONE");
		break;
	default:
		LOG_WRN("Unknown event");
		break;
	}
}

static uint8_t mcuboot_buf[256] __aligned(4);
static size_t m_fw_size = 0;

static int start(const struct hio_atci *atci, char *argv)
{
	if (!argv || strlen(argv) == 0) {
		return -EINVAL;
	}

	char *endptr;
	size_t size = strtoul(argv, &endptr, 10);
	if (*endptr != '\0' || size == 0) {
		hio_atci_error(atci, "\"Invalid parse size\"");
		return -EINVAL;
	}

	LOG_INF("Starting firmware update with size %zu", size);

	int ret = dfu_target_mcuboot_set_buf(mcuboot_buf, sizeof(mcuboot_buf));
	if (ret) {
		LOG_ERR("dfu_target_mcuboot_set_buf failed: %d", ret);
		return ret;
	}

	dfu_target_reset();

	ret = dfu_target_init(DFU_TARGET_IMAGE_TYPE_MCUBOOT, 0, size, dfu_target_callback_handler);
	if (ret) {
		LOG_ERR("dfu_target_init failed: %d", ret);
		if (ret == -EFBIG) {
			hio_atci_error(atci, "\"Image size too big\"");
		} else {
			hio_atci_error(atci, "\"Failed to initialize DFU target\"");
		}
		return ret;
	}

	m_fw_size = size;

	return 0;
}

static int chunk(const struct hio_atci *atci, char *argv)
{
	if (!argv || strlen(argv) == 0) {
		return -EINVAL;
	}

	const char *p = argv;
	bool def = false;
	uint32_t offset = 0;

	if (!(p = hio_tok_uint32(p, &def, &offset)) || !def) {
		hio_atci_error(atci, "\"Invalid offset\"");
		return -EINVAL;
	}

	if (!(p = hio_tok_sep(p))) {
		hio_atci_error(atci, "\"Invalid separator\"");
		return -EINVAL;
	}

	uint8_t buf[128];
	size_t len = 0;
	if (!(p = hio_tok_hex(p, &def, buf, sizeof(buf), &len)) || !def) {
		hio_atci_error(atci, "\"Invalid hex data\"");
		return -EINVAL;
	}

	if (!hio_tok_end(p)) {
		return -EINVAL;
	}

	LOG_INF("Offset %u with length %zu", offset, len);

	int ret = dfu_target_mcuboot_set_buf(mcuboot_buf, sizeof(mcuboot_buf));
	if (ret) {
		LOG_ERR("dfu_target_mcuboot_set_buf failed: %d", ret);
		return ret;
	}

	size_t dfu_offset;
	ret = dfu_target_offset_get(&dfu_offset);
	if (ret) {
		LOG_ERR("dfu_target_offset_get failed: %d", ret);
		hio_atci_error(atci, "\"Failed to get DFU target offset\"");
		return ret;
	}

	if (dfu_offset != offset) {
		LOG_ERR("DFU target offset mismatch: %zu != %u", dfu_offset, offset);
		hio_atci_errorf(atci, "\"DFU target offset mismatch: %zu != %u\"", dfu_offset,
				offset);
		return -EINVAL;
	}

	ret = dfu_target_write(buf, len);
	if (ret) {
		LOG_ERR("dfu_target_write failed: %d", ret);
		hio_atci_error(atci, "\"Failed to write DFU target\"");
		return ret;
	}

	int progress = (int)((dfu_offset + len) * 100 / m_fw_size);
	if (progress > 100) {
		progress = 100;
	}

	LOG_INF("Progress: %d%%", progress);

	return 0;
}

static int done(const struct hio_atci *atci)
{
	int ret = dfu_target_mcuboot_set_buf(mcuboot_buf, sizeof(mcuboot_buf));
	if (ret) {
		LOG_ERR("dfu_target_mcuboot_set_buf failed: %d", ret);
		hio_atci_error(atci, "\"Failed to set DFU target buffer\"");
		return ret;
	}

	size_t dfu_offset;
	ret = dfu_target_offset_get(&dfu_offset);
	if (ret) {
		LOG_ERR("dfu_target_offset_get failed: %d", ret);
		hio_atci_error(atci, "\"Failed to get DFU target offset\"");
		return ret;
	}

	if (dfu_offset != m_fw_size) {
		LOG_ERR("DFU target offset mismatch: %zu != %zu", dfu_offset, m_fw_size);
		hio_atci_errorf(atci, "\"DFU target offset mismatch: %zu != %zu\"", dfu_offset,
				m_fw_size);
		return -EINVAL;
	}

	ret = dfu_target_done(true);
	if (ret) {
		LOG_ERR("dfu_target_done failed: %d", ret);
		hio_atci_error(atci, "\"Failed to finalize DFU target\"");
		return ret;
	}

	ret = dfu_target_schedule_update(0);
	if (ret) {
		LOG_ERR("dfu_target_schedule_update failed: %d", ret);
		return ret;
	}

	LOG_INF("Firmware update scheduled");

	return 0;
}

static int confirm(const struct hio_atci *atci)
{
	int ret;
	if (boot_is_img_confirmed()) {
		LOG_INF("Image is already confirmed");
		hio_atci_error(atci, "\"Image is already confirmed\"");
		return -ECANCELED;
	}
	LOG_INF("Image is not confirmed");

	struct boot_swap_state swap_state;

	ret = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY, &swap_state);
	if (ret) {
		LOG_ERR("Failed to read swap state: %d", ret);
		hio_atci_errorf(atci, "\"Failed to read swap state: %d\"", ret);
		return ret;
	}

	if (swap_state.magic != BOOT_MAGIC_GOOD) {
		LOG_ERR("Image is not good, magic: %s", swap_state_magic_str(swap_state.magic));
		hio_atci_error(atci, "\"Image is not good\"");
		return -EINVAL;
	}

	if (swap_state.swap_type != BOOT_SWAP_TYPE_PERM &&
	    swap_state.swap_type != BOOT_SWAP_TYPE_TEST) {
		LOG_ERR("Image is not in a valid state for confirmation, swap type: %s",
			swap_type_str(swap_state.swap_type));
		hio_atci_error(atci, "\"Image is not in a valid state for confirmation\"");
		return -EINVAL;
	}

	if (boot_write_img_confirmed()) {
		LOG_ERR("Failed to write image confirmed flag");
		hio_atci_error(atci, "\"Failed to confirm image\"");
		return -EIO;
	}

	return 0;
}

static int at_fw_set(const struct hio_atci *atci, char *argv)
{
	if (!argv || strlen(argv) == 0) {
		return -EINVAL;
	}

	if (strcmp(argv, "\"info\"") == 0) {
		return info(atci);
	}

	if (strncmp(argv, "\"start\",", 8) == 0) {
		return start(atci, argv + 8);
	}

	if (strncmp(argv, "\"chunk\",", 8) == 0) {
		return chunk(atci, argv + 8);
	}

	if (strcmp(argv, "\"done\"") == 0) {
		return done(atci);
	}

	if (strcmp(argv, "\"confirm\"") == 0) {
		return confirm(atci);
	}

	return -EINVAL;
}

static int at_fw_read(const struct hio_atci *atci)
{
	hio_atci_printfln(atci, "$FW: \"confirmed\",%s",
			  boot_is_img_confirmed() ? "true" : "false");
	struct mcuboot_img_header hdr;
	boot_read_bank_header(FLASH_AREA_IMAGE_PRIMARY, &hdr, sizeof(hdr));
	hio_atci_printfln(atci, "$FW: \"version\",\"%u.%u.%u\"", hdr.h.v1.sem_ver.major,
			  hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision);
	hio_atci_printfln(atci, "$FW: \"swap type\",\"%s\"", swap_type_str(mcuboot_swap_type()));
	return 0;
}

HIO_ATCI_CMD_REGISTER(fw, "$FW", CONFIG_HIO_ATCI_CMD_FW_AUTH_FLAGS, NULL, at_fw_set, at_fw_read,
		      NULL, "Firmware command");
