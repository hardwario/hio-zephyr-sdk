/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_cloud.h>
#include <hio/hio_lte.h>

#include "hio_cloud_msg.h"

/* Zephyr includes */
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <string.h>

#include <zcbor_encode.h>

int hio_cloud_cbor_ncellmeas_put(zcbor_state_t *zs, const struct hio_lte_ncellmeas_param *param)
{
	if (!zs || !param) {
		return -EINVAL;
	}

	if (!param->valid) {
		zcbor_nil_put(zs, NULL);
		return 0;
	}
	zcbor_list_start_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);
	zcbor_uint32_put(zs, 1); /* version */
	zcbor_uint32_put(zs, param->act);
	zcbor_uint32_put(zs, param->num_cells);
	for (uint8_t i = 0; i < param->num_cells; i++) {
		const struct hio_lte_ncellmeas_cell_param *cell = &param->cells[i];
		zcbor_uint32_put(zs, cell->eci);
		zcbor_uint32_put(zs, cell->mcc);
		zcbor_uint32_put(zs, cell->mnc);
		zcbor_uint32_put(zs, cell->tac);
		zcbor_uint32_put(zs, cell->adv);
		zcbor_uint32_put(zs, cell->earfcn);
		zcbor_uint32_put(zs, cell->pci);
		zcbor_float32_put(zs, hio_lte_rsrp_idx_to_dbm(cell->rsrp));
		zcbor_float32_put(zs, hio_lte_rsrq_idx_to_db(cell->rsrq));
		/* Only the stored neighbors, so that the count is the length of the list */
		uint8_t first = MIN(cell->ncell_first, param->num_ncells);
		uint8_t count = MIN(cell->neighbor_count, param->num_ncells - first);

		zcbor_uint32_put(zs, count);
		for (uint8_t j = 0; j < count; j++) {
			const struct hio_lte_ncellmeas_ncell_param *ncell =
				&param->ncells[first + j];
			zcbor_uint32_put(zs, ncell->earfcn);
			zcbor_uint32_put(zs, ncell->pci);
			zcbor_float32_put(zs, hio_lte_rsrp_idx_to_dbm(ncell->rsrp));
			zcbor_float32_put(zs, hio_lte_rsrq_idx_to_db(ncell->rsrq));
			zcbor_int32_put(zs, ncell->time_diff);
		}
	}
	zcbor_list_end_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);

	return 0;
}

size_t hio_cloud_cbor_free(const zcbor_state_t *zs)
{
	return zs->payload_end - zs->payload_mut;
}

size_t hio_cloud_cbor_len(const zcbor_state_t *zs)
{
	return hio_cloud_msg_data_len(zs);
}

int hio_cloud_cbor_put_raw(zcbor_state_t *zs, const void *cbor, size_t len)
{
	if (!zs) {
		return -EINVAL;
	}

	if (!cbor || !len) {
		/* Also fail the state, so that ignoring the return value cannot send a bad message
		 */
		zcbor_error(zs, ZCBOR_ERR_WRONG_VALUE);
		return -EINVAL;
	}

	/* Same as zcbor with stop_on_error: nothing is written after the first error */
	int err = zcbor_peek_error(zs);

	if (err) {
		return hio_cloud_msg_zcbor_errno(err);
	}

	if (len > hio_cloud_cbor_free(zs)) {
		zcbor_error(zs, ZCBOR_ERR_NO_PAYLOAD);
		return -ENOSPC;
	}

	memcpy(zs->payload_mut, cbor, len);
	zs->payload_mut += len;
	zs->elem_count++;

	return 0;
}

void hio_cloud_cbor_put_key_ts(zcbor_state_t *zs, uint32_t key, int64_t ts)
{
	zcbor_uint32_put(zs, key);

	if (ts == 0) {
		zcbor_nil_put(zs, NULL);
	} else {
		zcbor_int64_put(zs, ts);
	}
}
