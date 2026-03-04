/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HARDWARIO includes */
#include <hio/hio_info.h>
#include <hio/hio_config.h>

/* Nordic includes */
#include <ncs_version.h>

#if defined(CONFIG_BUILD_WITH_TFM)
#include <tfm_ns_interface.h>
#include <tfm_ioctl_api.h>
#elif defined(CONFIG_SOC_SERIES_NRF52X)
#include <nrf52.h>
#elif defined(CONFIG_SOC_SERIES_NRF54LX)
#if NCS_VERSION_NUMBER < 0x30201
#include <nrf54l15.h>
#endif
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

static int m_state = -EAGAIN;

struct hio_info_data {
	char vendor_name[VENDOR_NAME_LENGTH];
	char product_name[PRODUCT_NAME_LENGTH];
	char hw_variant[HW_VARIANT_LENGTH];
	char hw_revision[HW_REVISION_LENGTH];
	char serial_number[SERIAL_NUMBER_LENGTH];
	char claim_token[CLAIM_TOKEN_LENGTH];
	char ble_passkey[BLE_PASSKEY_LENGTH];
};

static struct hio_info_data m_data = {
	.vendor_name = CONFIG_HIO_INFO_DEFAULT_VENDOR_NAME,
	.product_name = CONFIG_HIO_INFO_DEFAULT_PRODUCT_NAME,
	.hw_variant = CONFIG_HIO_INFO_DEFAULT_HW_VARIANT,
	.hw_revision = CONFIG_HIO_INFO_DEFAULT_HW_REVISION,
	.serial_number = CONFIG_HIO_INFO_DEFAULT_SERIAL_NUMBER,
	.claim_token = CONFIG_HIO_INFO_DEFAULT_CLAIM_TOKEN,
	.ble_passkey = CONFIG_HIO_INFO_DEFAULT_BLE_PASSKEY,
};
static uint32_t m_serial_number_uint32 = 0;

#if defined(APP_VERSION_STRING)
const char m_app_version[] = APP_VERSION_STRING;
#else
const char m_app_version[] = "(unset)";
#endif

#if IS_ENABLED(CONFIG_HIO_INFO_DEV_MODE)

#define SETTINGS_PFX "info"

static struct hio_info_data m_data_interim;

/* clang-format off */

static struct hio_config_item m_config_items[] = {
	HIO_CONFIG_ITEM_STRING("vendor-name",   m_data_interim.vendor_name,  "vendor name",       CONFIG_HIO_INFO_DEFAULT_VENDOR_NAME),
	HIO_CONFIG_ITEM_STRING("product-name",  m_data_interim.product_name, "product name",      CONFIG_HIO_INFO_DEFAULT_PRODUCT_NAME),
	HIO_CONFIG_ITEM_STRING("hw-variant",    m_data_interim.hw_variant,   "hardware variant",  CONFIG_HIO_INFO_DEFAULT_HW_VARIANT),
	HIO_CONFIG_ITEM_STRING("hw-revision",   m_data_interim.hw_revision,  "hardware revision", CONFIG_HIO_INFO_DEFAULT_HW_REVISION),
	HIO_CONFIG_ITEM_STRING("serial-number", m_data_interim.serial_number,"serial number",     CONFIG_HIO_INFO_DEFAULT_SERIAL_NUMBER),
	HIO_CONFIG_ITEM_STRING("claim-token",   m_data_interim.claim_token,  "claim token",       CONFIG_HIO_INFO_DEFAULT_CLAIM_TOKEN),
	HIO_CONFIG_ITEM_STRING("ble-passkey",   m_data_interim.ble_passkey,  "BLE passkey",       CONFIG_HIO_INFO_DEFAULT_BLE_PASSKEY),
};

/* clang-format on */

static int dev_config_init(void)
{
	static struct hio_config config = {
		.name = SETTINGS_PFX,
		.items = m_config_items,
		.nitems = ARRAY_SIZE(m_config_items),
		.interim = &m_data_interim,
		.final = &m_data,
		.size = sizeof(m_data),
	};

	return hio_config_register(&config);
}

#endif /* CONFIG_HIO_INFO_DEV_MODE */

static int load_pib(void)
{
	uint8_t pib[128];

#if defined(CONFIG_BUILD_WITH_TFM)
	uint32_t err = 0;
	enum tfm_platform_err_t plt_err;
	const uint32_t uicr_otp_start = NRF_UICR_S_BASE + offsetof(NRF_UICR_Type, OTP);
	plt_err = tfm_platform_mem_read(pib, uicr_otp_start, sizeof(pib), &err);
	if (plt_err != TFM_PLATFORM_ERR_SUCCESS || err != 0) {
		LOG_ERR("tfm_platform_mem_read failed: %d", err);
		return -EIO;
	}
#elif defined(CONFIG_SOC_SERIES_NRF52X)
	for (int i = 0; i < (ARRAY_SIZE(pib) / 4); i++) {
		((uint32_t *)pib)[i] = NRF_UICR->CUSTOMER[i];
	}
#elif defined(CONFIG_SOC_SERIES_NRF54LX)
	for (int i = 0; i < (ARRAY_SIZE(pib) / 4); i++) {
		((uint32_t *)pib)[i] = NRF_UICR->OTP[i];
	}
#else
	LOG_ERR("Unsupported SoC series");
	m_state = -ENOTSUP;
	return -ENOTSUP;
#endif

	LOG_HEXDUMP_DBG(pib, sizeof(pib), "PIB dump:");

	/* Load signature */
	uint32_t signature = sys_get_be32(pib + SIGNATURE_OFFSET);
	if (signature != SIGNATURE_VALUE) {
		LOG_WRN("Invalid signature: 0x%08x", signature);
		return -EINVAL;
	}

	/* Load version */
	uint8_t version = pib[VERSION_OFFSET];
	if (version != VERSION_VALUE) {
		LOG_WRN("Incompatible version: 0x%02x", version);
		return -EINVAL;
	}

	/* Load size */
	uint8_t size = pib[SIZE_OFFSET];
	if (size != SIZE_VALUE) {
		LOG_WRN("Unexpected size: 0x%02x", size);
		return -EINVAL;
	}

	/* Load CRC */
	uint32_t crc_stored = sys_get_be32(pib + CRC_OFFSET);

	/* Calculate CRC */
	uint32_t crc = crc32_ieee(pib, size - CRC_LENGTH);
	if (crc_stored != crc) {
		LOG_WRN("CRC mismatch: 0x%08x (read) 0x%08x (calculated)", crc_stored, crc);
		return -EINVAL;
	}

	/* Load vendor name */
	memcpy(m_data.vendor_name, pib + VENDOR_NAME_OFFSET, sizeof(m_data.vendor_name));

	/* Load product name */
	memcpy(m_data.product_name, pib + PRODUCT_NAME_OFFSET, sizeof(m_data.product_name));

	/* Load hardware variant */
	memcpy(m_data.hw_variant, pib + HW_VARIANT_OFFSET, sizeof(m_data.hw_variant));

	/* Load hardware revision */
	memcpy(m_data.hw_revision, pib + HW_REVISION_OFFSET, sizeof(m_data.hw_revision));

	/* Load serial number */
	memcpy(m_data.serial_number, pib + SERIAL_NUMBER_OFFSET, sizeof(m_data.serial_number));

	/* Load claim token */
	memcpy(m_data.claim_token, pib + CLAIM_TOKEN_OFFSET, sizeof(m_data.claim_token));

	/* Load BLE passkey */
	memcpy(m_data.ble_passkey, pib + BLE_PASSKEY_OFFSET, sizeof(m_data.ble_passkey));

	m_state = 0;

	return 0;
}

int hio_info_get_vendor_name(const char **vendor_name)
{
	*vendor_name = m_data.vendor_name;
	return m_state;
}

int hio_info_get_product_name(const char **product_name)
{
	*product_name = m_data.product_name;
	return m_state;
}

int hio_info_get_hw_variant(const char **hw_variant)
{
	*hw_variant = m_data.hw_variant;
	return m_state;
}

int hio_info_get_hw_revision(const char **hw_revision)
{
	*hw_revision = m_data.hw_revision;
	return m_state;
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
	*serial_number = m_data.serial_number;
	return m_state;
}

int hio_info_get_serial_number_uint32(uint32_t *serial_number)
{
	*serial_number = m_serial_number_uint32;

	if (m_state) {
		return m_state;
	}

	if (m_serial_number_uint32 == 0) {
		return -EINVAL;
	}

	return 0;
}

int hio_info_get_claim_token(const char **claim_token)
{
	*claim_token = m_data.claim_token;
	return m_state;
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
	*ble_passkey = m_data.ble_passkey;
	return m_state;
}

int hio_info_get_product_family(uint8_t *product_family)
{
	if (m_state) {
		return m_state;
	}

	if ((m_serial_number_uint32 & 0x80000000U) == 0) {
		return -EFAULT;
	}

	*product_family = (m_serial_number_uint32 & 0x3FF00000) >> 20;

	return 0;
}

static int init(void)
{
	LOG_INF("System initialization");

#if IS_ENABLED(CONFIG_HIO_INFO_DEV_MODE)
	LOG_WRN("Using development credentials from config");

	int ret = dev_config_init();
	if (ret < 0) {
		LOG_ERR("Call `dev_config_init` failed: %d", ret);
		return ret;
	}

	m_state = 0;
#else
	m_state = load_pib();
#endif

	if (m_state == 0) {
		char *endptr;
		m_serial_number_uint32 = strtoul(m_data.serial_number, &endptr, 10);
		if (endptr == m_data.serial_number || *endptr != '\0') {
			LOG_WRN("Invalid serial number: %s", m_data.serial_number);
			m_serial_number_uint32 = 0;
		}
	}

	return 0;
}

#if IS_ENABLED(CONFIG_HIO_INFO_DEV_MODE)
SYS_INIT(init, APPLICATION, 1);
#else
SYS_INIT(init, POST_KERNEL, CONFIG_HIO_INFO_INIT_PRIORITY);
#endif
