/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef CHESTER_INCLUDE_DRIVERS_ADC_ADS122C04EX_H_
#define CHESTER_INCLUDE_DRIVERS_ADC_ADS122C04EX_H_

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
enum ctr_x0_channel {
	CTR_X0_CHANNEL_1 = 0,
	CTR_X0_CHANNEL_2 = 1,
	CTR_X0_CHANNEL_3 = 2,
	CTR_X0_CHANNEL_4 = 3,
};*/

typedef int (*adc_ads122c04_api_read)(const struct device *dev, int32_t *result);
typedef int (*adc_ads122c04_api_set_mux)(const struct device *dev, int mux);

struct adc_ads122c04_driver_api {
	adc_ads122c04_api_read read;
	adc_ads122c04_api_set_mux set_mux;
};

static inline int adc_ads122c04_read(const struct device *dev, int32_t *result)
{
	const struct adc_ads122c04_driver_api *api =
		(const struct adc_ads122c04_driver_api *)dev->api;

	return api->read(dev, result);
}

static inline int adc_ads122c04_set_mux(const struct device *dev, int mux)
{
	const struct adc_ads122c04_driver_api *api =
		(const struct adc_ads122c04_driver_api *)dev->api;

	return api->set_mux(dev, mux);
}

#ifdef __cplusplus
}
#endif

#endif /* CHESTER_INCLUDE_DRIVERS_ADC_ADS122C04EX_H_ */
