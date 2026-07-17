/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_INCLUDE_CLOUD_PROCESS_H_
#define HIO_INCLUDE_CLOUD_PROCESS_H_

#include "hio_cloud_msg.h"

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_cloud.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int hio_cloud_process_dlconfig(struct hio_cloud_msg_dlconfig *msg);
int hio_cloud_process_dlshell(struct hio_cloud_msg_dlshell *msg, struct hio_buf *buf);
int hio_cloud_process_dlfirmware(struct hio_cloud_msg_dlfirmware *msg, struct hio_buf *buf);

/* Consistent snapshot of the firmware download progress into @p status.
 * status->running is true between the first accepted chunk and the reboot that
 * applies the update (cleared on any error so it never sticks). The remaining
 * fields (offset, size, id, target, type) are meaningful only while running.
 * Taken under a mutex so the whole snapshot is coherent. */
void hio_cloud_process_get_dfu_status(struct hio_cloud_dfu_status *status);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_PROCESS_H_ */
