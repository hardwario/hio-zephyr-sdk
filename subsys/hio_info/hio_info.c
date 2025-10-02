/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HARDWARIO includes */
#include <hio/hio_info.h>

/* Nordic includes */
#if defined(CONFIG_SOC_NRF52X)
#include <nrf52.h>
#elif defined(CONFIG_SOC_NRF9151_LACA)
#include <nrf9120.h>
#include <tfm_ns_interface.h>
#include <tfm_ioctl_api.h>
#elif defined(CONFIG_SOC_SERIES_NRF54LX)
#include <nrf54l15.h>
#else
#warning "Unsupported SoC series"
#endif

/* Build includes */
#include <app_version.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

LOG_MODULE_REGISTER(hio_info, CONFIG_HIO_INFO_LOG_LEVEL);

#define SIGNATURE_OFFSET 0x00
#define SIGNATURE_LENGTH 4
#define SIGNATURE_VALUE  0xbabecafe

#define VERSION_OFFSET 0x04
#define VERSION_LENGTH 1
#define VERSION_VALUE  2

#define SIZE_OFFSET 0x05
#define SIZE_LENGTH 1
#define SIZE_VALUE  123

#define VENDOR_NAME_OFFSET 0x06
#define VENDOR_NAME_LENGTH 17

#define PRODUCT_NAME_OFFSET 0x17
#define PRODUCT_NAME_LENGTH 17

#define HW_VARIANT_OFFSET 0x28
#define HW_VARIANT_LENGTH 11

#define HW_REVISION_OFFSET 0x33
#define HW_REVISION_LENGTH 7

#define FW_BUNDLE_LENGTH 128

#define FW_NAME_LENGTH 65

#define FW_VERSION_LENGTH 17

#define SERIAL_NUMBER_OFFSET 0x3a
#define SERIAL_NUMBER_LENGTH 11

#define CLAIM_TOKEN_OFFSET 0x45
#define CLAIM_TOKEN_LENGTH 33

#define BLE_PASSKEY_OFFSET 0x66
#define BLE_PASSKEY_LENGTH 17

#define CRC_OFFSET 0x77
#define CRC_LENGTH 4

static bool m_pib_valid;

static uint32_t m_signature;
static uint8_t m_version;
static uint8_t m_size;
static char m_vendor_name[VENDOR_NAME_LENGTH];
static char m_product_name[PRODUCT_NAME_LENGTH];
static char m_hw_variant[HW_VARIANT_LENGTH];
static char m_hw_revision[HW_REVISION_LENGTH];
static char m_serial_number[SERIAL_NUMBER_LENGTH];
static uint32_t m_serial_number_uint32;
static char m_claim_token[CLAIM_TOKEN_LENGTH];
static char m_ble_passkey[BLE_PASSKEY_LENGTH];
static uint32_t m_crc;

#if defined(APP_VERSION_STRING)
const char m_app_version[] = APP_VERSION_STRING;
#else
const char m_app_version[] = "(unset)";
#endif

static int load_pib(void)
{
	uint8_t pib[128];

#if defined(CONFIG_SOC_SERIES_NRF52X)
	for (int i = 0; i < (ARRAY_SIZE(pib) / 4); i++) {
		((uint32_t *)pib)[i] = NRF_UICR->CUSTOMER[i];
	}
#elif defined(CONFIG_SOC_SERIES_NRF54LX)
	for (int i = 0; i < (ARRAY_SIZE(pib) / 4); i++) {
		((uint32_t *)pib)[i] = NRF_UICR->OTP[i];
	}
#else
	uint32_t err = 0;
	enum tfm_platform_err_t plt_err;
	const uint32_t uicr_otp_start = NRF_UICR_S_BASE + offsetof(NRF_UICR_Type, OTP);
	plt_err = tfm_platform_mem_read(pib, uicr_otp_start, sizeof(pib), &err);
	if (plt_err != TFM_PLATFORM_ERR_SUCCESS || err != 0) {
		LOG_ERR("tfm_platform_mem_read failed: %d", err);
		return -1;
	}
#endif

	LOG_HEXDUMP_DBG(pib, sizeof(pib), "PIB dump:");

	/* Load signature */
	m_signature = sys_get_be32(pib + SIGNATURE_OFFSET);
	if (m_signature != SIGNATURE_VALUE) {
		LOG_WRN("Invalid signature: 0x%08x", m_signature);
		return -EINVAL;
	}

	/* Load version */
	m_version = pib[VERSION_OFFSET];
	if (m_version != VERSION_VALUE) {
		LOG_WRN("Incompatible version: 0x%02x", m_version);
		return -EINVAL;
	}

	/* Load size */
	m_size = pib[SIZE_OFFSET];
	if (m_size != SIZE_VALUE) {
		LOG_WRN("Unexpected size: 0x%02x", m_size);
		return -EINVAL;
	}

	/* Load CRC */
	m_crc = sys_get_be32(pib + CRC_OFFSET);

	/* Calculate CRC */
	uint32_t crc = crc32_ieee(pib, m_size - CRC_LENGTH);
	if (m_crc != crc) {
		LOG_WRN("CRC mismatch: 0x%08x (read) 0x%08x (calculated)", m_crc, crc);
		return -EINVAL;
	}

	/* Load vendor name */
	memcpy(m_vendor_name, pib + VENDOR_NAME_OFFSET, sizeof(m_vendor_name));

	/* Load product name */
	memcpy(m_product_name, pib + PRODUCT_NAME_OFFSET, sizeof(m_product_name));

	/* Load hardware variant */
	memcpy(m_hw_variant, pib + HW_VARIANT_OFFSET, sizeof(m_hw_variant));

	/* Load hardware revision */
	memcpy(m_hw_revision, pib + HW_REVISION_OFFSET, sizeof(m_hw_revision));

	/* Load serial number */
	memcpy(m_serial_number, pib + SERIAL_NUMBER_OFFSET, sizeof(m_serial_number));
	m_serial_number_uint32 = strtoul(m_serial_number, NULL, 10);

	/* Load claim token */
	memcpy(m_claim_token, pib + CLAIM_TOKEN_OFFSET, sizeof(m_claim_token));

	/* Load BLE passkey */
	memcpy(m_ble_passkey, pib + BLE_PASSKEY_OFFSET, sizeof(m_ble_passkey));

	m_pib_valid = true;

	return 0;
}

int hio_info_get_vendor_name(const char **vendor_name)
{
	if (!m_pib_valid) {
		*vendor_name = "(unset)";
		return -EIO;
	}

	*vendor_name = m_vendor_name;

	return 0;
}

int hio_info_get_product_name(const char **product_name)
{
	if (!m_pib_valid) {
		*product_name = "(unset)";
		return -EIO;
	}

	*product_name = m_product_name;

	return 0;
}

int hio_info_get_hw_variant(const char **hw_variant)
{
	if (!m_pib_valid) {
		*hw_variant = "(unset)";
		return -EIO;
	}

	*hw_variant = m_hw_variant;

	return 0;
}

int hio_info_get_hw_revision(const char **hw_revision)
{
	if (!m_pib_valid) {
		*hw_revision = "(unset)";
		return -EIO;
	}

	*hw_revision = m_hw_revision;

	return 0;
}

int hio_info_get_fw_bundle(const char **fw_bundle)
{
#if defined(FW_BUNDLE)
	int ret;
	static char buf[FW_BUNDLE_LENGTH];
	ret = snprintf(buf, sizeof(buf), "%s", STRINGIFY(FW_BUNDLE));
	if (ret != strlen(buf)) {
		return -ENOSPC;
	}

	*fw_bundle = buf;
#else
	*fw_bundle = "(unset)";
#endif

	return 0;
}

int hio_info_get_fw_name(const char **fw_name)
{
#if defined(FW_NAME)
	int ret;
	static char buf[FW_NAME_LENGTH];
	ret = snprintf(buf, sizeof(buf), "%s", STRINGIFY(FW_NAME));
	if (ret != strlen(buf)) {
		return -ENOSPC;
	}

	*fw_name = buf;
#else
	*fw_name = "(unset)";
#endif

	return 0;
}

int hio_info_get_fw_version(const char **fw_version)
{
	*fw_version = m_app_version;
	return 0;
}

int hio_info_get_serial_number(const char **serial_number)
{
	if (!m_pib_valid) {
		*serial_number = "(unset)";
		return -EIO;
	}

	*serial_number = m_serial_number;

	return 0;
}

int hio_info_get_serial_number_uint32(uint32_t *serial_number)
{
	if (!m_pib_valid) {
		*serial_number = 0;
		return -EIO;
	}

	*serial_number = m_serial_number_uint32;

	return 0;
}

int hio_info_get_claim_token(const char **claim_token)
{
	if (!m_pib_valid) {
		*claim_token = "(unset)";
		return -EIO;
	}

	*claim_token = m_claim_token;

	return 0;
}

int hio_info_get_ble_devaddr(const char **ble_devaddr)
{
	int ret;

	uint64_t devaddr;

	ret = hio_info_get_ble_devaddr_uint64(&devaddr);
	if (ret) {
		return ret;
	}

	uint8_t a[6] = {
		devaddr, devaddr >> 8, devaddr >> 16, devaddr >> 24, devaddr >> 32, devaddr >> 40,
	};

	static char buf[18];
	ret = snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2],
		       a[1], a[0]);
	if (ret != strlen(buf)) {
		return -ENOSPC;
	}

	*ble_devaddr = buf;

	return 0;
}

int hio_info_get_ble_devaddr_uint64(uint64_t *ble_devaddr)
{
#if defined(CONFIG_SOC_SERIES_NRF52X)
	*ble_devaddr = NRF_FICR->DEVICEADDR[1];
	*ble_devaddr &= BIT_MASK(16);
	*ble_devaddr |= BIT(15) | BIT(14);
	*ble_devaddr <<= 32;
	*ble_devaddr |= NRF_FICR->DEVICEADDR[0];
	return 0;
#else
	*ble_devaddr = 0;
	return -ENOTSUP;
#endif
}

int hio_info_get_ble_passkey(const char **ble_passkey)
{
	if (!m_pib_valid) {
		*ble_passkey = "(unset)";
		return -EIO;
	}

	*ble_passkey = m_ble_passkey;

	return 0;
}

int hio_info_get_product_family(uint8_t *product_family)
{
	if (!m_pib_valid) {
		return -EIO;
	}

	if ((m_serial_number_uint32 & 0x80000000) == 0) {
		return -EFAULT;
	}

	*product_family = (m_serial_number_uint32 & 0x3FF00000) >> 20;

	return 0;
}

static int init(void)
{
	LOG_INF("System initialization");

	load_pib();

	return 0;
}

SYS_INIT(init, POST_KERNEL, CONFIG_HIO_INFO_INIT_PRIORITY);
