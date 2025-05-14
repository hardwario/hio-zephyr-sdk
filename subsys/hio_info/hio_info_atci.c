/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HARDWARIO includes */
#include <hio/hio_info.h>
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

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
