/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include <zephyr/kernel.h>
#include <hio/hio_log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{

	LOG_INF("Starting application");

	while (1) {

		LOG_INF("uptime: %lld", k_uptime_get());
		LOG_DBG("uptime: %lld", k_uptime_get());
		LOG_WRN("uptime: %lld", k_uptime_get());

		k_sleep(K_SECONDS(1));
	}

	return 0;
}

HIO_LOG_SET_CURRENT_MODULE_LEVEL(LOG_LEVEL_INF);
