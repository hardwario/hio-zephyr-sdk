/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_process.h"
#include "hio_cloud_util.h"
#include "hio_cloud_msg.h"
#include "hio_cloud_transfer.h"

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/byteorder.h>

/* NCS includes */
#include <dfu/dfu_target.h>
#include <dfu/dfu_target_mcuboot.h>

/* HIO includes */
#include <hio/hio_config.h>
#include <hio/hio_sys.h>

/* Standard includes */
#include <tinycrypt/constants.h>
#include <tinycrypt/sha256.h>

LOG_MODULE_REGISTER(cloud_process, CONFIG_HIO_CLOUD_LOG_LEVEL);

int hio_cloud_process_dlconfig(struct hio_cloud_msg_dlconfig *config)
{
	LOG_INF("Received config: num lines: %d", config->lines);

	HIO_BUF_DEFINE(line, 0);

	int ret;

	const struct shell *sh = shell_backend_dummy_get_ptr();

	for (int i = 0; i < config->lines; i++) {
		hio_buf_reset(&line);

		ret = hio_cloud_msg_dlconfig_get_next_line(config, &line);
		if (ret) {
			LOG_ERR("Call `hio_cloud_msg_dlconfig_get_next_line` failed: %d", ret);
			return ret;
		}

		const char *cmd = hio_buf_get_mem(&line);

		LOG_INF("Command %d: %s", i, cmd);

		shell_backend_dummy_clear_output(sh);

		ret = shell_execute_cmd(sh, cmd);

		if (ret) {
			LOG_ERR("Failed to execute shell command: %s", cmd);
			return ret;
		}

		size_t size;

		const char *p = shell_backend_dummy_get_output(sh, &size);
		if (!p) {
			LOG_ERR("Failed to get shell output");
			return -ENOMEM;
		}

		LOG_INF("Shell output: %s", p);

		k_sleep(K_MSEC(10));
	}

	k_sleep(K_SECONDS(1));

	LOG_INF("Save config and reboot");

#if defined CONFIG_ZTEST
	return 0;
#endif

	k_sleep(K_SECONDS(1));

	ret = hio_config_save();
	if (ret) {
		LOG_ERR("Call `config_save` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_cloud_process_dlshell(struct hio_cloud_msg_dlshell *dlshell, struct hio_buf *buf)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);

	LOG_INF("Received shell: num cmds: %d", dlshell->commands);

	int ret;
	size_t size;
	struct hio_cloud_msg_upshell upshell;

	HIO_BUF_DEFINE(command, CONFIG_SHELL_CMD_BUFF_SIZE);

	hio_cloud_msg_pack_upshell_start(&upshell, buf, dlshell->message_id);

	for (int i = 0; i < dlshell->commands; i++) {
		hio_buf_reset(&command);
		ret = hio_cloud_msg_dlshell_get_next_command(dlshell, &command);
		if (ret) {
			LOG_ERR("Call `hio_cloud_msg_dlshell_get_next_command` failed: %d", ret);
			return ret;
		}

		shell_backend_dummy_clear_output(sh);
		const char *cmd = hio_buf_get_mem(&command);

		LOG_DBG("Execute command %d: %s", i, cmd);

		int result = shell_execute_cmd(sh, cmd);

		const char *output = (char *)shell_backend_dummy_get_output(sh, &size);
		if (!output) {
			LOG_ERR("Failed to get output");
			return -ENOMEM;
		}

		LOG_DBG("Shell output: %s", output);

		ret = hio_cloud_msg_pack_upshell_add_response(&upshell, cmd, result, output);
		if (ret) {
			LOG_ERR("Call `cloud_msg_pack_upshell_add_cmd` failed: %d", ret);
			return ret;
		}
	}

	ret = hio_cloud_msg_pack_upshell_end(&upshell);
	if (ret) {
		LOG_ERR("Call `hio_cloud_msg_pack_upshell_end` failed: %d", ret);
		return ret;
	}

	return 0;
}

#if CONFIG_DFU_TARGET_MCUBOOT
static void dfu_target_callback_handler(enum dfu_target_evt_id evt)
{
	switch (evt) {
	case DFU_TARGET_EVT_TIMEOUT:
		LOG_INF("DFU_TARGET_EVT_TIMEOUT");
		break;
	case DFU_TARGET_EVT_ERASE_DONE:
		LOG_INF("DFU_TARGET_EVT_ERASE_DONE");
		break;
	default:
		LOG_INF("Unknown event");
		break;
	}
}
#endif

int hio_cloud_process_dlfirmware(struct hio_cloud_msg_dlfirmware *dlfirmware, struct hio_buf *buf)
{
	int ret;

	LOG_INF("Received firmware: target: %s, type: %s, offset: %d, length: %d",
		dlfirmware->target, dlfirmware->type, dlfirmware->offset, dlfirmware->length);
	if (dlfirmware->firmware_size) {
		LOG_INF("Firmware size: %d", dlfirmware->firmware_size);
	}

	if (strcmp(dlfirmware->target, "app") != 0) {
		LOG_ERR("Unsupported target: %s", dlfirmware->target);
		return -EINVAL;
	}

	size_t offset = 0;

	if (strcmp(dlfirmware->type, "chunk") == 0) {
		if (dlfirmware->firmware_size == 0) {
			LOG_ERR("Firmware size is 0");
			return -EINVAL;
		}

#if CONFIG_DFU_TARGET_MCUBOOT
		static uint8_t mcuboot_buf[256] __aligned(4);

		ret = dfu_target_mcuboot_set_buf(mcuboot_buf, sizeof(mcuboot_buf));
		if (ret) {
			LOG_ERR("dfu_target_mcuboot_set_buf failed: %d", ret);
			return ret;
		}

		if (dlfirmware->offset == 0) {
			dfu_target_reset();

			ret = dfu_target_init(DFU_TARGET_IMAGE_TYPE_MCUBOOT, 0,
					      dlfirmware->firmware_size,
					      dfu_target_callback_handler);
			if (ret) {
				LOG_ERR("dfu_target_init failed: %d", ret);

				struct hio_cloud_upfirmware upfirmware = {
					.target = "app",
					.type = "error",
					.id = dlfirmware->id,
					.offset = offset,
					.error = "image size too big"};

				ret = hio_cloud_msg_pack_firmware(buf, &upfirmware);
				if (ret) {
					LOG_ERR("hio_cloud_msg_pack_firmware failed: %d", ret);
					return ret;
				}

				return ret;
			}
		} else {
			ret = dfu_target_offset_get(&offset);
			if (ret) {
				LOG_ERR("dfu_target_offset_get failed: %d", ret);

				if (ret == -EACCES) {
					struct hio_cloud_upfirmware upfirmware = {
						.target = "app",
						.type = "error",
						.id = dlfirmware->id,
						.offset = offset,
						.error = "offset mismatch (device was rebooted)"};

					ret = hio_cloud_msg_pack_firmware(buf, &upfirmware);
					if (ret) {
						LOG_ERR("hio_cloud_msg_pack_firmware failed: %d",
							ret);
						return ret;
					}

					return 0;
				}

				return ret;
			}
		}
#else
		LOG_ERR("Unsupported MCUBOOT: %s", dlfirmware->type);

		struct hio_cloud_upfirmware upfirmware = {.target = "app",
							  .type = "error",
							  .id = dlfirmware->id,
							  .offset = offset,
							  .error = "unsupported MCUBOOT"};

		ret = hio_cloud_msg_pack_firmware(buf, &upfirmware);
		if (ret) {
			LOG_ERR("hio_cloud_msg_pack_firmware failed: %d", ret);
			return ret;
		}

		return 0;
#endif

	} else {
		LOG_ERR("Unsupported type: %s", dlfirmware->type);
		return -EINVAL;
	}

	if (offset != dlfirmware->offset) {
		LOG_ERR("Invalid offset: %d, expected: %d", offset, dlfirmware->offset);
		return -EINVAL;
	}

	ret = dfu_target_write(dlfirmware->data, dlfirmware->length);
	if (ret) {
		LOG_ERR("dfu_target_write failed: %d", ret);
		return ret;
	}

	ret = dfu_target_offset_get(&offset);
	if (ret) {
		LOG_ERR("dfu_target_offset_get failed: %d", ret);
		return ret;
	}

	if (offset == dlfirmware->firmware_size) {
		ret = dfu_target_done(true);
		if (ret) {
			LOG_ERR("dfu_target_done failed: %d", ret);
			return ret;
		}

		ret = dfu_target_schedule_update(0);
		if (ret) {
			LOG_ERR("dfu_target_schedule_update failed: %d", ret);
			return ret;
		}

		LOG_INF("Firmware update scheduled");

		struct hio_cloud_upfirmware upfirmware = {
			.target = "app",
			.type = "swap",
			.id = dlfirmware->id,
			.offset = offset,
		};

		ret = hio_cloud_msg_pack_firmware(buf, &upfirmware);
		if (ret) {
			LOG_ERR("hio_cloud_msg_pack_firmware failed: %d", ret);
			return ret;
		}

		ret = hio_cloud_transfer_uplink(buf, NULL);
		if (ret) {
			LOG_ERR("Call `hio_cloud_transfer_uplink` for upbuf failed: %d", ret);
			return ret;
		}

		hio_cloud_util_save_firmware_update_id(upfirmware.id);

		LOG_INF("Reboot to apply firmware update");

#if defined CONFIG_ZTEST
		return 0;
#endif
		k_sleep(K_MSEC(100));

		hio_sys_reboot("Firmware update");

	} else {
		LOG_DBG("Firmware next offset: %d", offset);

		struct hio_cloud_upfirmware upfirmware = {
			.target = "app",
			.type = "next",
			.id = dlfirmware->id,
			.offset = offset,
			.max_length = ((HIO_CLOUD_TRANSFER_BUF_SIZE - 50) / 256) * 256,
		};

		ret = hio_cloud_msg_pack_firmware(buf, &upfirmware);
		if (ret) {
			LOG_ERR("hio_cloud_msg_pack_firmware failed: %d", ret);
			return ret;
		}

		LOG_DBG("Send next firmware");
	}

	return 0;
}
