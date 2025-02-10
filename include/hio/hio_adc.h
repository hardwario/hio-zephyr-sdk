/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef INCLUDE_HIO_ADC_H_
#define INCLUDE_HIO_ADC_H_

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_adc hio_adc
 * @{
 */

/* Default ADC gain is 1/6 */
#define HIO_ADC_MILLIVOLTS(_sample)        (((uint32_t)(_sample)) * 600 * 6 / 4095)
#define HIO_ADC_MILLIVOLTS_GAIN_1(_sample) (((uint32_t)(_sample)) * 600 * 1 / 4095)

/* X0 contains 100kΩ and 10kΩ voltage divider when PD is enabled */
#define HIO_ADC_X0_AI_MILLIVOLTS(_sample)                                                          \
	((float)HIO_ADC_MILLIVOLTS(_sample) * ((100.f + 10.f) / 10.f))

#define HIO_ADC_X0_AI_NODIV_MILLIVOLTS(_sample) ((float)HIO_ADC_MILLIVOLTS(_sample))

/* X0 contains 249Ω ohm measurement resistor and then 100kΩ and 10kΩ voltage divider */
#define HIO_ADC_X0_CL_MILLIAMPS(_sample)                                                           \
	((float)HIO_ADC_MILLIVOLTS(_sample) * ((100.f + 10.f) / 10.f) / 249.f)

int hio_adc_init(uint8_t channel);
int hio_adc_read(uint8_t channel, uint16_t *sample);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_ADC_H_ */
