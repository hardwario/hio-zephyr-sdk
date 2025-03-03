/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef INCLUDE_HIO_THERM_H_
#define INCLUDE_HIO_THERM_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_therm hio_therm
 * @{
 */

int hio_therm_read(float *temperature);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_THERM_H_ */
