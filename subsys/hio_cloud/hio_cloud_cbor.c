/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_cloud.h>

/* Standard includes */
#include <errno.h>

#include <zcbor_encode.h>

static inline float rsrp_idx_to_dbm(int16_t rsrp)
{
	return (rsrp) < 0 ? (rsrp)-140 : (rsrp)-141;
}

static inline float rsrq_idx_to_db(int16_t rsrq)
{
	if ((rsrq) < 0) {
		return ((float)(rsrq)-39) * 0.5f;
	} else if ((rsrq) < 35) {
		return ((float)(rsrq)-40) * 0.5f;
	} else {
		return ((float)(rsrq)-41) * 0.5f;
	}
}

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
		zcbor_float32_put(zs, rsrp_idx_to_dbm(cell->rsrp));
		zcbor_float32_put(zs, rsrq_idx_to_db(cell->rsrq));
		zcbor_uint32_put(zs, cell->neighbor_count);
		for (uint8_t j = 0; j < cell->neighbor_count; j++) {
			const struct hio_lte_ncellmeas_ncell_param *ncell = &cell->ncells[j];
			zcbor_uint32_put(zs, ncell->earfcn);
			zcbor_uint32_put(zs, ncell->pci);
			zcbor_float32_put(zs, rsrp_idx_to_dbm(ncell->rsrp));
			zcbor_float32_put(zs, rsrq_idx_to_db(ncell->rsrq));
			zcbor_int32_put(zs, ncell->time_diff);
		}
	}
	zcbor_list_end_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);

	return 0;
}
