/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Link stubs for the hio_cloud.c externals. Only the init handshake (CREATE
 * SESSION answered by SET SESSION) and the packets of the guarded functions
 * are functional; config/decoder/encoder uploads are skipped (no decoder and
 * encoder set, the config hash matches the session). */

#include "hio_cloud_config.h"
#include "hio_cloud_msg.h"
#include "hio_cloud_process.h"

/* HIO includes */
#include <hio/hio_buf.h>
#include <hio/hio_cloud.h>
#include <hio/hio_config.h>
#include <hio/hio_info.h>
#include <hio/hio_rtc.h>

/* Standard includes */
#include <errno.h>
#include <string.h>

int hio_cloud_msg_pack_create_session(struct hio_buf *buf)
{
	return hio_buf_append_u8(buf, UL_CREATE_SESSION);
}

int hio_cloud_msg_unpack_set_session(struct hio_buf *buf, struct hio_cloud_session *session)
{
	memset(session, 0, sizeof(*session));
	session->id = 1;

	return 0;
}

int hio_cloud_msg_unpack_set_timestamp(struct hio_buf *buf, int64_t *timestamp)
{
	return -ENOTSUP;
}

int hio_cloud_msg_pack_decoder(struct hio_buf *buf, uint64_t hash, const uint8_t *decoder_buf,
			       size_t decoder_len)
{
	return -ENOTSUP;
}

int hio_cloud_msg_pack_encoder(struct hio_buf *buf, uint64_t hash, const uint8_t *encoder_buf,
			       size_t encoder_len)
{
	return -ENOTSUP;
}

int hio_cloud_msg_pack_config(struct hio_buf *buf)
{
#if TEST_CONFIG_PACK_FAIL
	/* e.g. the config does not fit */
	return -ENOSPC;
#else
	return 0;
#endif
}

int hio_cloud_msg_unpack_config(struct hio_buf *buf, struct hio_cloud_msg_dlconfig *config)
{
	return -ENOTSUP;
}

int hio_cloud_msg_get_hash(struct hio_buf *buf, uint64_t *hash)
{
	/* Same as the session config hash: no config upload */
	*hash = 0;

	return 0;
}

int hio_cloud_msg_config_hash(uint64_t *hash)
{
	return -ENOTSUP;
}

int hio_cloud_msg_unpack_dlshell(struct hio_buf *buf, struct hio_cloud_msg_dlshell *msg)
{
	return -ENOTSUP;
}

int hio_cloud_msg_pack_firmware(struct hio_buf *buf, const struct hio_cloud_upfirmware *upfirmware)
{
	/* A real packet type, different from the data packet, so that a nested
	 * hio_cloud_firmware_update() that gets through visibly breaks the message */
	int ret = hio_buf_append_u8(buf, UL_UPLOAD_FIRMWARE);

	if (ret) {
		return ret;
	}

	if (!upfirmware->firmware) {
		/* The ack of an update: its id, for the test to check */
		return hio_buf_append_mem(buf, upfirmware->id, sizeof(hio_cloud_uuid_t));
	}

	return hio_buf_append_str(buf, upfirmware->firmware);
}

int hio_cloud_msg_unpack_dlfirmware(struct hio_buf *buf,
				    struct hio_cloud_msg_dlfirmware *dlfirmware)
{
	return -ENOTSUP;
}

int hio_cloud_process_dlconfig(struct hio_cloud_msg_dlconfig *msg)
{
	return -ENOTSUP;
}

int hio_cloud_process_dlshell(struct hio_cloud_msg_dlshell *msg, struct hio_buf *buf)
{
	return -ENOTSUP;
}

int hio_cloud_process_dlfirmware(struct hio_cloud_msg_dlfirmware *msg, struct hio_buf *buf)
{
	return -ENOTSUP;
}

void hio_cloud_process_get_dfu_status(struct hio_cloud_dfu_status *status)
{
	memset(status, 0, sizeof(*status));
}

int hio_cloud_config_init(void)
{
	return 0;
}

int hio_info_get_serial_number_uint32(uint32_t *serial_number)
{
	*serial_number = 1;

	return 0;
}

int hio_info_get_claim_token(const char **claim_token)
{
	*claim_token = "00112233445566778899aabbccddeeff";

	return 0;
}

int hio_rtc_set_ts(int64_t ts)
{
	return 0;
}

int hio_rtc_get_ts(int64_t *ts)
{
	*ts = 0;

	return 0;
}

uint32_t hio_config_get_gen(void)
{
	return 0;
}

bool hio_config_is_dirty(void)
{
	return false;
}

int hio_config_iter_modules(hio_config_module_cb_t cb, void *user_data)
{
	return 0;
}
