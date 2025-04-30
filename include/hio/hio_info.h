/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */
#ifndef INCLUDE_HIO_INFO_H_
#define INCLUDE_HIO_INFO_H_

/* Standard includes */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_info hio_info
 * @{
 */

int hio_info_get_vendor_name(const char **vendor_name);
int hio_info_get_product_name(const char **product_name);
int hio_info_get_hw_variant(const char **hw_variant);
int hio_info_get_hw_revision(const char **hw_revision);
int hio_info_get_fw_bundle(const char **fw_bundle);
int hio_info_get_fw_name(const char **fw_name);
int hio_info_get_fw_version(const char **fw_version);
int hio_info_get_serial_number(const char **serial_number);
int hio_info_get_serial_number_uint32(uint32_t *serial_number);
int hio_info_get_product_family(uint8_t *product_family);
int hio_info_get_claim_token(const char **claim_token);
int hio_info_get_ble_devaddr(const char **ble_devaddr);
int hio_info_get_ble_devaddr_uint64(uint64_t *ble_devaddr);
int hio_info_get_ble_passkey(const char **ble_passkey);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_INFO_H_ */
