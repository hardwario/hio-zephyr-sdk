/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_cloud_packet.h"

/* HIO includes */
#include <hio/hio_buf.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <tinycrypt/constants.h>
#include <tinycrypt/sha256.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_cloud_packet, CONFIG_HIO_CLOUD_LOG_LEVEL);

static int calculate_packet_hash(uint8_t packet_hash[8], uint8_t claim_token[16],
				 const uint8_t *buf, size_t len)
{
	int ret;

	struct tc_sha256_state_struct s;
	ret = tc_sha256_init(&s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_init` failed: %d", ret);
		return ret;
	}

	ret = tc_sha256_update(&s, claim_token, 16);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_update` failed: %d", ret);
		return ret;
	}

	ret = tc_sha256_update(&s, buf, len);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_update` failed: %d", ret);
		return ret;
	}

	uint8_t digest[32];
	ret = tc_sha256_final(digest, &s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_final` failed: %d", ret);
		return ret;
	}

	for (int i = 0; i < 8; i++) {
		packet_hash[i] = digest[i] ^ digest[8 + i] ^ digest[16 + i] ^ digest[24 + i];
	}

#if 0
	LOG_HEXDUMP_INF(claim_token, 16, "Claim token:");
	LOG_HEXDUMP_INF(buf, len, "Buffer:");
	LOG_HEXDUMP_INF(&digest[0], 8, "Digest 1:");
	LOG_HEXDUMP_INF(&digest[8], 8, "Digest 2:");
	LOG_HEXDUMP_INF(&digest[16], 8, "Digest 3:");
	LOG_HEXDUMP_INF(&digest[24], 8, "Digest 4:");
	LOG_HEXDUMP_INF(packet_hash, 8, "Packet hash:");
#endif

	return 0;
}

int hio_cloud_packet_pack(struct hio_cloud_packet *pck, uint8_t claim_token[16],
			  struct hio_buf *buf)
{
	if (pck->data_len > HIO_CLOUD_DATA_MAX_SIZE) {
		LOG_ERR("Data is too large: %zu", pck->data_len);
		return -EINVAL;
	}

	if (pck->sequence > 0x0FFF) {
		LOG_ERR("Sequence number is too large: %u", pck->sequence);
		return -EINVAL;
	}

	int ret;

	hio_buf_reset(buf);
	hio_buf_seek(buf, HIO_CLOUD_PACKET_MIN_SIZE);

	if (pck->data_len > 0) {
		ret = hio_buf_append_mem(buf, pck->data, pck->data_len);
		if (ret) {
			LOG_ERR("Call `hio_buf_append_mem` failed: %d", ret);
			return ret;
		}
	}

	size_t used = hio_buf_get_used(buf);

	ret = hio_buf_seek(buf, 8);
	if (ret) {
		LOG_ERR("Call `hio_buf_seek` failed: %d", ret);
		return ret;
	}

	ret = hio_buf_append_u32_be(buf, pck->serial_number);
	if (ret) {
		LOG_ERR("Call `hio_buf_append_u32_be` failed: %d", ret);
		return ret;
	}

	uint16_t header = ((uint16_t)pck->flags) << 12 | pck->sequence;

	ret = hio_buf_append_u16_be(buf, header);
	if (ret) {
		LOG_ERR("Call `hio_buf_append_u16_be` failed: %d", ret);
		return ret;
	}

	uint8_t packet_hash[8];
	ret = calculate_packet_hash(packet_hash, claim_token, hio_buf_get_mem(buf) + 8, used - 8);
	if (ret) {
		LOG_ERR("Call `calculate_packet_hash` failed: %d", ret);
		return ret;
	}

	ret = hio_buf_seek(buf, 0);
	if (ret) {
		LOG_ERR("Call `hio_buf_seek` failed: %d", ret);
		return ret;
	}

	ret = hio_buf_append_mem(buf, packet_hash, sizeof(packet_hash));
	if (ret) {
		LOG_ERR("Call `hio_buf_append_mem` failed: %d", ret);
		return ret;
	}

	ret = hio_buf_seek(buf, used);
	if (ret) {
		LOG_ERR("Call `hio_buf_seek` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_cloud_packet_unpack(struct hio_cloud_packet *pck, uint8_t claim_token[16],
			    struct hio_buf *buf)
{
	int ret;

	if (hio_buf_get_used(buf) < HIO_CLOUD_PACKET_MIN_SIZE) {
		LOG_ERR("Packet is too short");
		return -EBADMSG;
	}

	uint8_t packet_hash[8];
	ret = calculate_packet_hash(packet_hash, claim_token, hio_buf_get_mem(buf) + 8,
				    hio_buf_get_used(buf) - 8);
	if (ret) {
		LOG_ERR("Call `calculate_packet_hash` failed: %d", ret);
		return ret;
	}

	if (memcmp(packet_hash, hio_buf_get_mem(buf), sizeof(packet_hash))) {
		LOG_ERR("Packet hash mismatch");
		return -EBADMSG;
	}

	pck->serial_number = sys_get_be32(hio_buf_get_mem(buf) + 8);

	uint16_t header = sys_get_be16(hio_buf_get_mem(buf) + 12);

	pck->flags = (header >> 12) & BIT_MASK(4);
	pck->sequence = header & BIT_MASK(12);
	pck->data = hio_buf_get_mem(buf) + HIO_CLOUD_PACKET_MIN_SIZE;
	pck->data_len = hio_buf_get_used(buf) - HIO_CLOUD_PACKET_MIN_SIZE;

	return 0;
}

const char *hio_cloud_packet_flags_to_str(uint8_t flags)
{
	static char flag_str[7];

	flag_str[0] = '[';
	flag_str[1] = (flags & HIO_CLOUD_PACKET_FLAG_FIRST) ? 'F' : 'x';
	flag_str[2] = (flags & HIO_CLOUD_PACKET_FLAG_LAST) ? 'L' : 'x';
	flag_str[3] = (flags & HIO_CLOUD_PACKET_FLAG_ACK) ? 'A' : 'x';
	flag_str[4] = (flags & HIO_CLOUD_PACKET_FLAG_POLL) ? 'P' : 'x';
	flag_str[5] = ']';
	flag_str[6] = '\0';

	return flag_str;
}

uint16_t hio_cloud_packet_sequence_inc(uint16_t sequence)
{
	sequence++;
	if (sequence == 4096) {
		sequence = 1;
	}
	return sequence;
}
