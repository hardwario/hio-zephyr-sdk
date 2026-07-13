/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef SUBSYS_HIO_CLOUD_BACKEND_H_
#define SUBSYS_HIO_CLOUD_BACKEND_H_

#include "hio_cloud_transfer.h"

/* HIO includes */
#include <hio/hio_buf.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal transport seam for hio_cloud. NOT a public API: it lives here, not
 * in include/hio/. The seam operates on whole cloud buffers (uplink/downlink);
 * FLAP fragmentation, sequence handling, RAI and address failover are internal
 * details of the udp_lte backend below.
 */

struct hio_cloud_backend_failover_state {
	int active_idx;            /* index of the active address */
	const char *active_addr;   /* active address (string) */
	int consecutive_failures;  /* current failure counter */
	uint32_t failover_count;   /* number of switches since boot */
};

struct hio_cloud_backend {
	int (*init)(uint32_t serial_number, const uint8_t token[16]);
	int (*wait_ready)(k_timeout_t timeout);
	/* Deliver the whole uplink buffer. */
	int (*uplink)(struct hio_buf *buf, bool *has_downlink, k_timeout_t timeout);
	/* Fetch the whole downlink buffer. */
	int (*downlink)(struct hio_buf *buf, bool *has_downlink, k_timeout_t timeout);
	int (*set_psk)(const char *psk_hex);
	int (*get_metrics)(struct hio_cloud_transfer_metrics *metrics);
	int (*reset_metrics)(void);
	/* -ENOTSUP if the transport has no failover. */
	int (*get_failover_state)(struct hio_cloud_backend_failover_state *state);
};

/* The only transport today: UDP over LTE, implemented by hio_cloud_transfer.c. */
extern const struct hio_cloud_backend hio_cloud_backend_udp_lte;

/* Active backend selected by hio_cloud. Callers outside hio_cloud.c reach the
 * transport through this accessor rather than binding to a concrete ops table. */
const struct hio_cloud_backend *hio_cloud_backend_get(void);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_CLOUD_BACKEND_H_ */
