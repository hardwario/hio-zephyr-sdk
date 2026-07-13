/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_INCLUDE_CLOUD_UTIL_H_
#define HIO_INCLUDE_CLOUD_UTIL_H_

#include "hio_cloud_msg.h"

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_cloud.h>

/* Crypto includes */
#if IS_ENABLED(CONFIG_HIO_CLOUD_HASH_MBEDTLS)
#include <mbedtls/sha256.h>
#elif IS_ENABLED(CONFIG_HIO_CLOUD_HASH_PSA)
#include <psa/crypto.h>
#else
#include <tinycrypt/constants.h>
#include <tinycrypt/sha256.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Incremental variant of hio_cloud_calculate_hash (XOR-folded SHA-256).
 */
struct hio_cloud_hash {
#if IS_ENABLED(CONFIG_HIO_CLOUD_HASH_MBEDTLS)
	mbedtls_sha256_context ctx;
#elif IS_ENABLED(CONFIG_HIO_CLOUD_HASH_PSA)
	psa_hash_operation_t op;
#else
	struct tc_sha256_state_struct s;
#endif
};

int hio_cloud_hash_begin(struct hio_cloud_hash *h);
int hio_cloud_hash_update(struct hio_cloud_hash *h, const void *data, size_t len);
int hio_cloud_hash_finish(struct hio_cloud_hash *h, uint8_t hash[8]);

int hio_cloud_calculate_hash(uint8_t hash[8],
			     const uint8_t *buf1, size_t len1,
			     const uint8_t *buf2, size_t len2);

int hio_cloud_util_shell_cmd(const char *cmd, struct hio_buf *buf);

int hio_cloud_util_uuid_to_str(const hio_cloud_uuid_t uuid, char *str, size_t len);

int hio_cloud_util_str_to_uuid(const char *str, hio_cloud_uuid_t uuid);

int hio_cloud_util_save_firmware_update_id(const hio_cloud_uuid_t uuid);

int hio_cloud_util_get_firmware_update_id(hio_cloud_uuid_t uuid);

int hio_cloud_util_delete_firmware_update_id(void);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_UTIL_H_ */
