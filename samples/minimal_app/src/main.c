/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_info.h>
#include <hio/hio_log.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{
	int ret;
	const char *fw_version;

	ret = hio_info_get_fw_version(&fw_version);
	if (ret) {
		LOG_ERR("Call `hio_info_get_fw_version` failed: %d", ret);
		return ret;
	}

	LOG_INF("Starting application (version: %s)", fw_version);

	for (;;) {
		LOG_INF("Alive, uptime: %lld ms", k_uptime_get());
		k_sleep(K_SECONDS(60));
	}

	return 0;
}
