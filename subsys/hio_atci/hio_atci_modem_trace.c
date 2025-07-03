/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_atci_io.h"
#include "hio_atci_modem_trace.h"

/* HIO includes */
#include <hio/hio_atci.h>

/* Nordic includes */
#include <modem/nrf_modem_lib_trace.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(hio_atci_modem_trace, CONFIG_HIO_ATCI_LOG_LEVEL);

#define BASE64_ENCODED_SIZE(x)   ((((x) + 2) / 3) * 4)
#define BASE64_MAX_INPUT_SIZE(x) ((((x) / 4) * 3) - 2)

#define BUFFER_SIZE 96

#if BUFFER_SIZE > BASE64_MAX_INPUT_SIZE(CONFIG_HIO_ATCI_CMD_BUFF_SIZE)
#error "BUFFER_SIZE is too large for the base64 encoding. Please reduce it."
#endif

#if !defined(CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_RAM)
#error "Modem trace backend RAM is not enabled. Please enable it in prj.conf. CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_RAM=y"
#endif

int process(const struct hio_atci *atci)
{
	static uint8_t buf[BUFFER_SIZE];

	int len = nrf_modem_lib_trace_read(buf, sizeof(buf));

	if (len < 0) {
		if (len == -1) {
			return 0; // Modem trace backend is not initialized
		}
		LOG_ERR("Modem trace read failed: %d", len);
		return len; // Error occurred
	}

	if (len == 0) {
		return 0; // No data to process
	}

	// LOG_INF("nrf_modem_lib_trace_data_size: %zu", nrf_modem_lib_trace_data_size());

	size_t olen = 0;
	int ret = base64_encode(atci->ctx->tmp_buff, sizeof(atci->ctx->tmp_buff), &olen, buf, len);
	if (ret) {
		LOG_ERR("Base64 encoding failed: %d", ret);
		return ret;
	}
	atci->ctx->tmp_buff[olen] = '\0'; // Null-terminate the string

	uint8_t crc_mode = atci->ctx->crc_mode;
	atci->ctx->crc_mode = 0;

	size_t size = nrf_modem_lib_trace_data_size();

	hio_atci_io_writef(atci, "@MT: %zu,\"%s\"", size, atci->ctx->tmp_buff);
	hio_atci_io_endline(atci);

	atci->ctx->crc_mode = crc_mode;

	return size;
}

int hio_atci_modem_trace_process(const struct hio_atci *atci)
{
	int ret = 0;
	do {
		ret = process(atci);
	} while (ret > 1000);

	return ret;
}
