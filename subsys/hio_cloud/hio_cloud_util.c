/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_util.h"

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
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/byteorder.h>

/* HIO includes */
#include <tinycrypt/constants.h>
#include <tinycrypt/sha256.h>

LOG_MODULE_REGISTER(cloud_util, CONFIG_HIO_CLOUD_LOG_LEVEL);

int hio_cloud_calculate_hash(uint8_t hash[8], const uint8_t *buf, size_t len)
{
	int ret;

	struct tc_sha256_state_struct s;
	ret = tc_sha256_init(&s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_init` failed: %d", ret);
		return ret;
	}

	ret = tc_sha256_update(&s, buf, len);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_update` failed: %d", ret);
		return ret;
	}

	uint8_t digest[32];
	ret = tc_sha256_final(digest, &s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_final` failed: %d", ret);
		return ret;
	}

	for (int i = 0; i < 8; i++) {
		hash[i] = digest[i] ^ digest[8 + i] ^ digest[16 + i] ^ digest[24 + i];
	}

	return 0;
}

int hio_cloud_util_shell_cmd(const char *cmd, struct hio_buf *buf)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	int ret;

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

	ret = hio_buf_append_mem(buf, p, size);
	if (ret) {
		LOG_ERR("Failed to append shell output to buffer");
		return ret;
	}

	ret = hio_buf_append_u8(buf, '\0');
	if (ret) {
		LOG_ERR("Failed to append null terminator to buffer");
		return ret;
	}

	return 0;
}

int hio_cloud_util_uuid_to_str(const hio_cloud_uuid_t uuid, char *str, size_t len)
{
	if (len < 37) {
		LOG_ERR("Buffer too small");
		return -EINVAL;
	}

	snprintf(str, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8],
		 uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);

	return 0;
}

int hio_cloud_util_str_to_uuid(const char *str, hio_cloud_uuid_t uuid)
{
	char temp[33];
	int i, j = 0;
	for (i = 0; str[i] != '\0'; i++) {
		if (str[i] != '-') {
			temp[j++] = str[i];
		}
	}
	temp[j] = '\0';

	if (strlen(temp) != 32) {
		return -1;
	}

	for (i = 0; i < 16; i++) {
		if (sscanf(temp + 2 * i, "%2hhx", &uuid[i]) != 1) {
			return -1;
		}
	}

	return 0;
}

int hio_cloud_util_save_firmware_update_id(const hio_cloud_uuid_t uuid)
{
	return settings_save_one("cloud/firmware/update_id", uuid, sizeof(hio_cloud_uuid_t));
}

struct settings_read_callback_params {
	void *data;
	size_t len;
};

static int settings_read_callback(const char *key, size_t len, settings_read_cb read_cb,
				  void *cb_arg, void *param)
{
	struct settings_read_callback_params *params = param;

	if (params->len < len) {
		return -EINVAL;
	}

	if (settings_name_next(key, NULL) != 0) {
		return -EINVAL;
	}

	if (read_cb(cb_arg, params->data, len) < 0) {
		return -EINVAL;
	}

	return 0;
}

int hio_cloud_util_get_firmware_update_id(hio_cloud_uuid_t uuid)
{
	struct settings_read_callback_params params = {
		.data = uuid,
		.len = sizeof(hio_cloud_uuid_t),
	};
	return settings_load_subtree_direct("cloud/firmware/update_id", settings_read_callback,
					    &params);
}

int hio_cloud_util_delete_firmware_update_id(void)
{
	return settings_delete("cloud/firmware/update_id");
}
