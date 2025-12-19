/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_INCLUDE_CLOUD_TRANSFER_H_
#define HIO_INCLUDE_CLOUD_TRANSFER_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

/* HIO includes */
#include <hio/hio_buf.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hio_cloud_transfer_metrics {
	uint32_t uplink_count;
	uint32_t uplink_bytes;
	uint32_t uplink_fragments;
	uint32_t uplink_errors;
	int64_t uplink_last_ts;

	uint32_t downlink_count;
	uint32_t downlink_fragments;
	uint32_t downlink_bytes;
	uint32_t downlink_errors;
	int64_t downlink_last_ts;

	uint32_t poll_count;
	int64_t poll_last_ts;
};

int hio_cloud_transfer_init(uint32_t serial_number, uint8_t token[16]);
int hio_cloud_transfer_wait_for_ready(k_timeout_t timeout);
int hio_cloud_transfer_reset_metrics(void);
int hio_cloud_transfer_get_metrics(struct hio_cloud_transfer_metrics *metrics);
int hio_cloud_transfer_uplink(struct hio_buf *buf, bool *has_downlink);
int hio_cloud_transfer_downlink(struct hio_buf *buf, bool *has_downlink);
int hio_cloud_transfer_set_psk(const char *psk_hex);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_TRANSFER_H_ */
