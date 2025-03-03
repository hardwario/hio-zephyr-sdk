/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef INCLUDE_HIO_ACCEL_H_
#define INCLUDE_HIO_ACCEL_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_accel hio_accel
 * @{
 */

int hio_accel_read(float *accel_x, float *accel_y, float *accel_z, int *orientation);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_ACCEL_H_ */
