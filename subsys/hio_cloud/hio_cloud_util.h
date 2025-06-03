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

#ifdef __cplusplus
extern "C" {
#endif

int hio_cloud_calculate_hash(uint8_t hash[8], const uint8_t *buf, size_t len);

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
