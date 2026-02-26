/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HARDWARIO includes */
#include <hio/hio_info.h>
#include <hio/hio_atci.h>
#include <hio/hio_tok.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Vendor name: KSB
// Product name: KSB Guard CLM
// Hardware variant:
// Hardware revision: R1.0
// Firmware bundle: aaa.bbb.ccc.ddd
// Firmware name: KSB Guard CLM
// Firmware version: 1.0.0
// Serial number: 3186622466

// Claim token: 3d723252154482ccc9ebfecbd7b7f13c
// BLE passkey: 123456

static int at_i_action(const struct hio_atci *atci)
{
	const char *product_name;
	hio_info_get_product_name(&product_name);
	const char *hw_variant;
	hio_info_get_hw_variant(&hw_variant);
	const char *hw_revision;
	hio_info_get_hw_revision(&hw_revision);

	hio_atci_printfln(atci, "\"%s%s%s-%s\"", product_name, strlen(hw_variant) ? "-" : "",
			  hw_variant, hw_revision);
	return 0;
}
HIO_ATCI_CMD_REGISTER(i, "I", 0, at_i_action, NULL, NULL, NULL, "Request product information");

static int at_cgmi_action(const struct hio_atci *atci)
{
	const char *vendor_name;
	hio_info_get_vendor_name(&vendor_name);
	hio_atci_printfln(atci, "+CGMI: \"%s\"", vendor_name);
	return 0;
}
HIO_ATCI_CMD_REGISTER(cgmi, "+CGMI", 0, at_cgmi_action, NULL, NULL, NULL,
		      "Request manufacturer name");

static int at_cgmm_action(const struct hio_atci *atci)
{
	const char *product_name;
	hio_info_get_product_name(&product_name);
	hio_atci_printfln(atci, "+CGMM: \"%s\"", product_name);
	return 0;
}
HIO_ATCI_CMD_REGISTER(cgmm, "+CGMM", 0, at_cgmm_action, NULL, NULL, NULL,
		      "Request model identification");

static int at_cgmr_action(const struct hio_atci *atci)
{
	const char *hw_revision;
	hio_info_get_hw_revision(&hw_revision);

	hio_atci_printfln(atci, "+CGMR: \"%s\"", hw_revision);
	return 0;
}
HIO_ATCI_CMD_REGISTER(cgmr, "+CGMR", 0, at_cgmr_action, NULL, NULL, NULL,
		      "Request revision identification");

static int at_cgsn_action(const struct hio_atci *atci)
{
	const char *serial_number;
	hio_info_get_serial_number(&serial_number);
	hio_atci_printfln(atci, "+CGSN: \"%s\"", serial_number);
	return 0;
}
HIO_ATCI_CMD_REGISTER(cgsn, "+CGSN", 0, at_cgsn_action, NULL, NULL, NULL,
		      "Request product serial number");

struct info_item {
	const char *name;
	const char *label;
	int (*getter)(const char **value);
};

static const struct info_item info_items[] = {
	{"vendor-name", "Vendor", hio_info_get_vendor_name},
	{"product-name", "Product", hio_info_get_product_name},
	{"hardware-variant", "HW Variant", hio_info_get_hw_variant},
	{"hardware-revision", "HW Revision", hio_info_get_hw_revision},
	{"firmware-bundle", "FW Bundle", hio_info_get_fw_bundle},
	{"firmware-name", "FW Name", hio_info_get_fw_name},
	{"firmware-version", "FW Version", hio_info_get_fw_version},
	{"serial-number", "Serial Number", hio_info_get_serial_number},
	{"claim-token", "Claim Token", hio_info_get_claim_token},
};

static void info_item_print(const struct hio_atci *atci, const struct info_item *item)
{
	const char *value;
	item->getter(&value);
	hio_atci_printfln(atci, "$INFO: \"%s\",\"%s\"", item->name, value);
}

static int at_info_set(const struct hio_atci *atci, char *argv)
{
	char *tmp;
	size_t tmp_len;
	hio_atci_get_tmp_buff(atci, &tmp, &tmp_len);
	const char *p = argv;
	bool def = false;

	if (!(p = hio_tok_str(p, &def, tmp, tmp_len)) || !def) {
		return -EINVAL;
	}

	if (!hio_tok_end(p)) {
		return -EINVAL;
	}

	for (int i = 0; i < ARRAY_SIZE(info_items); i++) {
		if (strcmp(tmp, info_items[i].name) == 0) {
			info_item_print(atci, &info_items[i]);
			return 0;
		}
	}

	hio_atci_error(atci, "\"Item not found\"");
	return -ENOENT;
}

static int at_info_read(const struct hio_atci *atci)
{
	for (int i = 0; i < ARRAY_SIZE(info_items); i++) {
		info_item_print(atci, &info_items[i]);
	}

	return 0;
}

static int at_info_test(const struct hio_atci *atci)
{
	hio_atci_printfln(atci, "$INFO: \"key\",\"type\",\"label\"");

	for (int i = 0; i < ARRAY_SIZE(info_items); i++) {
		hio_atci_printfln(atci, "$INFO: \"%s\",\"string\",\"%s\"",
				  info_items[i].name, info_items[i].label);
	}

	return 0;
}

HIO_ATCI_CMD_REGISTER(info, "$INFO", 0, NULL, at_info_set, at_info_read, at_info_test,
		      "Request device information");
