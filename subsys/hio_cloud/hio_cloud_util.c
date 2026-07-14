/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_util.h"

/* Nordic includes */
#include <ncs_version.h>

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

LOG_MODULE_REGISTER(cloud_util, CONFIG_HIO_CLOUD_LOG_LEVEL);

int hio_cloud_hash_begin(struct hio_cloud_hash *h)
{
#if IS_ENABLED(CONFIG_HIO_CLOUD_HASH_MBEDTLS)
	int ret;

	mbedtls_sha256_init(&h->ctx);
	ret = mbedtls_sha256_starts(&h->ctx, 0);
	if (ret) {
		LOG_ERR("Call `mbedtls_sha256_starts` failed: %d", ret);
		mbedtls_sha256_free(&h->ctx);
		return -EINVAL;
	}
#elif IS_ENABLED(CONFIG_HIO_CLOUD_HASH_PSA)
	psa_status_t status;

#if NCS_VERSION_NUMBER >= 0x30400
	/* PSA_HASH_OPERATION_INIT is an empty initializer `{ }` here, which is
	 * not valid in an assignment */
	h->op = psa_hash_operation_init();
#else
	h->op = PSA_HASH_OPERATION_INIT;
#endif
	status = psa_hash_setup(&h->op, PSA_ALG_SHA_256);
	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_hash_setup` failed: %d", status);
		return -EINVAL;
	}
#else
	int ret;

	ret = tc_sha256_init(&h->s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_init` failed: %d", ret);
		return -EINVAL;
	}
#endif

	return 0;
}

int hio_cloud_hash_update(struct hio_cloud_hash *h, const void *data, size_t len)
{
#if IS_ENABLED(CONFIG_HIO_CLOUD_HASH_MBEDTLS)
	int ret;

	ret = mbedtls_sha256_update(&h->ctx, data, len);
	if (ret) {
		LOG_ERR("Call `mbedtls_sha256_update` failed: %d", ret);
		mbedtls_sha256_free(&h->ctx);
		return -EINVAL;
	}
#elif IS_ENABLED(CONFIG_HIO_CLOUD_HASH_PSA)
	psa_status_t status;

	status = psa_hash_update(&h->op, data, len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_hash_update` failed: %d", status);
		psa_hash_abort(&h->op);
		return -EINVAL;
	}
#else
	int ret;

	ret = tc_sha256_update(&h->s, data, len);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_update` failed: %d", ret);
		return -EINVAL;
	}
#endif

	return 0;
}

int hio_cloud_hash_finish(struct hio_cloud_hash *h, uint8_t hash[8])
{
	uint8_t digest[32];

#if IS_ENABLED(CONFIG_HIO_CLOUD_HASH_MBEDTLS)
	int ret;

	ret = mbedtls_sha256_finish(&h->ctx, digest);
	if (ret) {
		LOG_ERR("Call `mbedtls_sha256_finish` failed: %d", ret);
		mbedtls_sha256_free(&h->ctx);
		return -EINVAL;
	}
	mbedtls_sha256_free(&h->ctx);
#elif IS_ENABLED(CONFIG_HIO_CLOUD_HASH_PSA)
	psa_status_t status;
	size_t hash_len;

	status = psa_hash_finish(&h->op, digest, sizeof(digest), &hash_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_hash_finish` failed: %d", status);
		psa_hash_abort(&h->op);
		return -EINVAL;
	}
#else
	int ret;

	ret = tc_sha256_final(digest, &h->s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_final` failed: %d", ret);
		return -EINVAL;
	}
#endif

	for (int i = 0; i < 8; i++) {
		hash[i] = digest[i] ^ digest[8 + i] ^ digest[16 + i] ^ digest[24 + i];
	}

	return 0;
}

void hio_cloud_hash_abort(struct hio_cloud_hash *h)
{
#if IS_ENABLED(CONFIG_HIO_CLOUD_HASH_MBEDTLS)
	mbedtls_sha256_free(&h->ctx);
#elif IS_ENABLED(CONFIG_HIO_CLOUD_HASH_PSA)
	psa_hash_abort(&h->op);
#else
	/* TinyCrypt keeps no external resources tied to the state struct. */
#endif
}

int hio_cloud_calculate_hash(uint8_t hash[8],
			     const uint8_t *buf1, size_t len1,
			     const uint8_t *buf2, size_t len2)
{
	int ret;
	struct hio_cloud_hash h;

	ret = hio_cloud_hash_begin(&h);
	if (ret) {
		return ret;
	}

	ret = hio_cloud_hash_update(&h, buf1, len1);
	if (ret) {
		return ret;
	}

	if (buf2 != NULL && len2 > 0) {
		ret = hio_cloud_hash_update(&h, buf2, len2);
		if (ret) {
			return ret;
		}
	}

	return hio_cloud_hash_finish(&h, hash);
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
	bool found;
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

	params->found = true;

	return 0;
}

int hio_cloud_util_get_firmware_update_id(hio_cloud_uuid_t uuid)
{
	struct settings_read_callback_params params = {
		.data = uuid,
		.len = sizeof(hio_cloud_uuid_t),
		.found = false,
	};

	/* settings_load_subtree_direct returns 0 even when the key does not
	 * exist (the callback is simply never invoked), so the caller cannot
	 * tell "loaded" from "absent" by the return value alone. Track it via
	 * the callback and report -ENOENT, otherwise the uninitialized uuid
	 * buffer would be taken as a valid id. */
	int ret = settings_load_subtree_direct("cloud/firmware/update_id",
					       settings_read_callback, &params);
	if (ret) {
		return ret;
	}

	return params.found ? 0 : -ENOENT;
}

int hio_cloud_util_delete_firmware_update_id(void)
{
	return settings_delete("cloud/firmware/update_id");
}
