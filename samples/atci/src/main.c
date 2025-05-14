/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include <hio/hio_atci.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* nRF includes */
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static int at_test_action(const struct hio_atci *atci)
{
	LOG_INF("AT+TEST action");
	hio_atci_printfln(atci, "$TEST: %d", 123);
	return 0;
}

HIO_ATCI_CMD_REGISTER(at_test, "$TEST", 0, at_test_action, NULL, NULL, NULL, "App test function.");

int main(void)
{
	// turn off modem

	int ret = nrf_modem_lib_init();
	if (ret) {
		LOG_ERR("Call `nrf_modem_lib_init` failed: %d", ret);
		return ret;
	}

	while (1) {
		ret = lte_lc_power_off();
		if (ret) {
			LOG_ERR("Call `lte_lc_power_off` failed: %d", ret);
			return ret;
		}

		LOG_INF("uptime: %lld", k_uptime_get());

		hio_atci_broadcastf("$UPTIME: %lld", k_uptime_get());

		k_sleep(K_SECONDS(10));
	}

	return 0;
}
