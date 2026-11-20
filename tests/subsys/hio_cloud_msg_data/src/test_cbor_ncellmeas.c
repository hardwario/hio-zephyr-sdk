/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_lte_parse.h"

/* HIO includes */
#include <hio/hio_cloud.h>
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/ztest.h>

/* Third-party includes */
#include <zcbor_decode.h>
#include <zcbor_encode.h>

/* Standard includes */
#include <string.h>

ZTEST_SUITE(hio_cloud_cbor_ncellmeas, NULL, NULL, NULL, NULL, NULL);

/* Two cells, with 2 and 1 neighbors */
#define URC_TWO_CELLS                                                                              \
	"0,\"00011B07\",\"26295\",\"00B7\",10512,9034,2300,7,63,31,150344527,1,2,"                 \
	"2300,8,60,29,92,2400,10,56,27,162,"                                                       \
	"\"00011B08\",\"23003\",\"8DCC\",65535,0,3544,9,62,30,150345527,0,1,"                      \
	"6300,11,40,20,10"

static uint8_t m_cbor[256];
static zcbor_state_t m_zd[4];

/* Not on the stack, so that the overwrite of the original is not optimized out */
static struct hio_lte_ncellmeas_param m_parsed;
static struct hio_lte_ncellmeas_param m_copy;

/* Encodes the measurement and starts the decoding of its list */
static void encode(const struct hio_lte_ncellmeas_param *param, uint32_t num_cells)
{
	zcbor_state_t zs[4];
	uint32_t v;

	memset(m_cbor, 0, sizeof(m_cbor));
	zcbor_new_encode_state(zs, ARRAY_SIZE(zs), m_cbor, sizeof(m_cbor), 1);
	zassert_ok(hio_cloud_cbor_ncellmeas_put(zs, param));
	zassert_ok(zcbor_peek_error(zs));

	zcbor_new_decode_state(m_zd, ARRAY_SIZE(m_zd), m_cbor, zs->payload - m_cbor, 1, NULL, 0);
	zassert_true(zcbor_list_start_decode(m_zd));
	zassert_true(zcbor_uint32_decode(m_zd, &v));
	zassert_equal(v, 1, "version");
	zassert_true(zcbor_uint32_decode(m_zd, &v)); /* act */
	zassert_true(zcbor_uint32_decode(m_zd, &v));
	zassert_equal(v, num_cells);
}

/* Decodes a cell up to its neighbor count, which it returns */
static uint32_t decode_cell(uint32_t eci)
{
	uint32_t v;
	float f;

	zassert_true(zcbor_uint32_decode(m_zd, &v));
	zassert_equal(v, eci);
	for (int i = 0; i < 6; i++) { /* mcc, mnc, tac, adv, earfcn, pci */
		zassert_true(zcbor_uint32_decode(m_zd, &v));
	}
	zassert_true(zcbor_float32_decode(m_zd, &f)); /* rsrp */
	zassert_true(zcbor_float32_decode(m_zd, &f)); /* rsrq */
	zassert_true(zcbor_uint32_decode(m_zd, &v));

	return v;
}

static void decode_ncell(uint32_t earfcn, uint32_t pci, float rsrp, float rsrq, int32_t time_diff)
{
	uint32_t v;
	float f;
	int32_t i;

	zassert_true(zcbor_uint32_decode(m_zd, &v));
	zassert_equal(v, earfcn);
	zassert_true(zcbor_uint32_decode(m_zd, &v));
	zassert_equal(v, pci);
	zassert_true(zcbor_float32_decode(m_zd, &f));
	zassert_equal(f, rsrp);
	zassert_true(zcbor_float32_decode(m_zd, &f));
	zassert_equal(f, rsrq);
	zassert_true(zcbor_int32_decode(m_zd, &i));
	zassert_equal(i, time_diff);
}

/* The state and the getter hand out copies: the parsed original is gone by the encode. */
ZTEST(hio_cloud_cbor_ncellmeas, test_neighbors_of_a_copy)
{
	zassert_ok(hio_lte_parse_urc_ncellmeas(URC_TWO_CELLS, 5, &m_parsed));
	memcpy(&m_copy, &m_parsed, sizeof(m_copy));
	memset(&m_parsed, 0, sizeof(m_parsed));

	encode(&m_copy, 2);
	zassert_equal(decode_cell(0x00011B07), 2);
	decode_ncell(2300, 8, -81.0f, -5.5f, 92);
	decode_ncell(2400, 10, -85.0f, -6.5f, 162);
	zassert_equal(decode_cell(0x00011B08), 1);
	decode_ncell(6300, 11, -101.0f, -10.0f, 10);
	zassert_true(zcbor_list_end_decode(m_zd));
}

/* Neighbors not stored are not written: each count is the length of its list. */
ZTEST(hio_cloud_cbor_ncellmeas, test_only_stored_neighbors)
{
	static const struct hio_lte_ncellmeas_param param = {
		.valid = true,
		.num_cells = 3,
		.num_ncells = 1,
		.cells = {{.eci = 1, .ncell_first = 0, .neighbor_count = 3},
			  {.eci = 2, .ncell_first = 1, .neighbor_count = 2},
			  {.eci = 3, .ncell_first = 9, .neighbor_count = 1}},
		.ncells = {{.earfcn = 6300, .pci = 11, .rsrp = 40, .rsrq = 20, .time_diff = 10}},
	};

	encode(&param, 3);
	zassert_equal(decode_cell(1), 1);
	decode_ncell(6300, 11, -101.0f, -10.0f, 10);
	zassert_equal(decode_cell(2), 0);
	zassert_equal(decode_cell(3), 0);
	zassert_true(zcbor_list_end_decode(m_zd));
}
