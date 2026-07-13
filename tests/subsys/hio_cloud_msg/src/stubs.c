/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Link stubs for hio_cloud_msg.c / hio_config.c externals not exercised by
 * these tests. */

#include <hio/hio_info.h>
#include <hio/hio_lte.h>
#include <hio/hio_sys.h>

#include <zephyr/sys/__assert.h>

#include <string.h>

void hio_sys_reboot(const char *reason)
{
	__ASSERT(false, "unexpected reboot: %s", reason);
}

int hio_info_get_vendor_name(const char **vendor_name)
{
	*vendor_name = "TEST";
	return 0;
}

int hio_info_get_product_name(const char **product_name)
{
	*product_name = "TEST";
	return 0;
}

int hio_info_get_hw_variant(const char **hw_variant)
{
	*hw_variant = "TEST";
	return 0;
}

int hio_info_get_hw_revision(const char **hw_revision)
{
	*hw_revision = "TEST";
	return 0;
}

int hio_info_get_fw_bundle(const char **fw_bundle)
{
	*fw_bundle = "TEST";
	return 0;
}

int hio_info_get_fw_name(const char **fw_name)
{
	*fw_name = "TEST";
	return 0;
}

int hio_info_get_fw_version(const char **fw_version)
{
	*fw_version = "TEST";
	return 0;
}

int hio_info_get_serial_number_uint32(uint32_t *serial_number)
{
	*serial_number = 0;
	return 0;
}

int hio_lte_get_imei(uint64_t *imei)
{
	*imei = 0;
	return 0;
}

int hio_lte_get_imsi(uint64_t *imsi)
{
	*imsi = 0;
	return 0;
}

int hio_lte_get_iccid(char **iccid)
{
	*iccid = "0";
	return 0;
}

int hio_lte_get_conn_param(struct hio_lte_conn_param *param)
{
	memset(param, 0, sizeof(*param));
	return 0;
}
