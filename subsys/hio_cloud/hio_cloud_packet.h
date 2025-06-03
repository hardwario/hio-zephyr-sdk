/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_INCLUDE_CLOUD_PACKET_H_
#define HIO_INCLUDE_CLOUD_PACKET_H_

/* HIO includes */
#include <hio/hio_buf.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIO_CLOUD_PACKET_HASH_SIZE          8
#define HIO_CLOUD_PACKET_SERIAL_NUMBER_SIZE 4
#define HIO_CLOUD_PACKET_HEADER_SIZE        2
#define HIO_CLOUD_PACKET_MIN_SIZE                                                                  \
	(HIO_CLOUD_PACKET_HASH_SIZE + HIO_CLOUD_PACKET_SERIAL_NUMBER_SIZE +                        \
	 HIO_CLOUD_PACKET_HEADER_SIZE)
#define HIO_CLOUD_PACKET_MAX_SIZE 508
#define HIO_CLOUD_DATA_MAX_SIZE   (HIO_CLOUD_PACKET_MAX_SIZE - HIO_CLOUD_PACKET_MIN_SIZE)

#define HIO_CLOUD_PACKET_FLAG_FIRST 0x08
#define HIO_CLOUD_PACKET_FLAG_LAST  0x04
#define HIO_CLOUD_PACKET_FLAG_ACK   0x02
#define HIO_CLOUD_PACKET_FLAG_POLL  0x01

struct hio_cloud_packet {
	uint32_t serial_number;
	uint16_t sequence;
	uint8_t flags;
	uint8_t *data;
	size_t data_len;
};

int hio_cloud_packet_pack(struct hio_cloud_packet *pck, uint8_t claim_token[16],
			  struct hio_buf *buf);

int hio_cloud_packet_unpack(struct hio_cloud_packet *pck, uint8_t claim_token[16],
			    struct hio_buf *buf);

const char *hio_cloud_packet_flags_to_str(uint8_t flags);

uint16_t hio_cloud_packet_sequence_inc(uint16_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_PACKET_H_ */
