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

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int hio_cloud_process_dlconfig(struct hio_cloud_msg_dlconfig *msg);
int hio_cloud_process_dlshell(struct hio_cloud_msg_dlshell *msg, struct hio_buf *buf);
int hio_cloud_process_dlfirmware(struct hio_cloud_msg_dlfirmware *msg, struct hio_buf *buf);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_PROCESS_H_ */
