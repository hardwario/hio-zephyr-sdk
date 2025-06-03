/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_INCLUDE_CLOUD_MSG_H_
#define HIO_INCLUDE_CLOUD_MSG_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_cloud.h>

#include <zcbor_common.h>

/* Zephyr includes */
#include <zephyr/shell/shell.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UL_CREATE_SESSION  0x00
#define UL_GET_TIMESTAMP   0x01
#define UL_UPLOAD_CONFIG   0x02
#define UL_UPLOAD_DECODER  0x03
#define UL_UPLOAD_ENCODER  0x04
#define UL_UPLOAD_STATS    0x05
#define UL_UPLOAD_DATA     0x06
#define UL_UPLOAD_SHELL    0x07
#define UL_UPLOAD_FIRMWARE 0x08

#define DL_SET_SESSION       0x80
#define DL_SET_TIMESTAMP     0x81
#define DL_DOWNLOAD_CONFIG   0x82
#define DL_DOWNLOAD_DATA     0x86
#define DL_DOWNLOAD_SHELL    0x87
#define DL_DOWNLOAD_FIRMWARE 0x88

#define DL_REQUEST_REBOOT 0xff

typedef uint8_t hio_cloud_uuid_t[16];

struct hio_cloud_msg_dlconfig {
	int lines;
	struct hio_buf *buf;
	zcbor_state_t zs[3];
};

struct hio_cloud_msg_dlshell {
	int commands;
	struct hio_buf *buf;
	zcbor_state_t zs[3 + 2];
	hio_cloud_uuid_t message_id;
};

struct hio_cloud_msg_upshell {
	struct hio_buf *buf;
	zcbor_state_t zs[2];
};

struct hio_cloud_msg_dlfirmware {
	char target[8];
	char type[10];
	hio_cloud_uuid_t id;
	uint32_t offset;
	uint32_t length;
	const uint8_t *data;
	uint32_t firmware_size;
};

struct hio_cloud_upfirmware {
	char target[8];
	char type[10];
	uint8_t *id;
	uint32_t offset;
	uint32_t max_length;
	const char *firmware;
	const char *error;
};

int hio_cloud_msg_pack_create_session(struct hio_buf *buf);
int hio_cloud_msg_unpack_set_session(struct hio_buf *buf, struct hio_cloud_session *session);

int hio_cloud_msg_pack_get_timestamp(struct hio_buf *buf);
int hio_cloud_msg_unpack_set_timestamp(struct hio_buf *buf, int64_t *timestamp);

int hio_cloud_msg_pack_decoder(struct hio_buf *buf, uint64_t hash, const uint8_t *decoder_buf,
			       size_t decoder_len);
int hio_cloud_msg_pack_encoder(struct hio_buf *buf, uint64_t hash, const uint8_t *encoder_buf,
			       size_t encoder_len);

int hio_cloud_msg_pack_stats(struct hio_buf *buf);

int hio_cloud_msg_pack_config(struct hio_buf *buf);
int hio_cloud_msg_unpack_config(struct hio_buf *buf, struct hio_cloud_msg_dlconfig *config);

int hio_cloud_msg_get_hash(struct hio_buf *buf, uint64_t *hash);

// for working with hio_cloud_msg_dlconfig
int hio_cloud_msg_dlconfig_reset(struct hio_cloud_msg_dlconfig *config);
int hio_cloud_msg_dlconfig_get_next_line(struct hio_cloud_msg_dlconfig *config,
					 struct hio_buf *line);

int hio_cloud_msg_unpack_dlshell(struct hio_buf *buf, struct hio_cloud_msg_dlshell *msg);
int hio_cloud_msg_dlshell_reset(struct hio_cloud_msg_dlshell *dlshell);
int hio_cloud_msg_dlshell_get_next_command(struct hio_cloud_msg_dlshell *dlshell,
					   struct hio_buf *command);

int hio_cloud_msg_pack_upshell_start(struct hio_cloud_msg_upshell *upshell, struct hio_buf *buf,
				     const hio_cloud_uuid_t message_id);
int hio_cloud_msg_pack_upshell_add_response(struct hio_cloud_msg_upshell *upshell,
					    const char *command, const int result,
					    const char *output);
int hio_cloud_msg_pack_upshell_end(struct hio_cloud_msg_upshell *upshell);

int hio_cloud_msg_pack_firmware(struct hio_buf *buf, const struct hio_cloud_upfirmware *upfirmware);
int hio_cloud_msg_unpack_dlfirmware(struct hio_buf *buf,
				    struct hio_cloud_msg_dlfirmware *dlfirmware);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_MSG_H_ */
