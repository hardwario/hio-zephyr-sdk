/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_atci.h>
#include <hio/hio_cloud.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(hio_cloud_atci, CONFIG_HIO_CLOUD_LOG_LEVEL);

static int at_confighash_read(const struct hio_atci *atci)
{
	int ret;
	uint64_t saved, cloud;

	/* Get all hashes first so that a failure does not leave a partial response */
	ret = hio_cloud_get_config_hash(&saved);
	if (ret) {
		LOG_ERR("Call `hio_cloud_get_config_hash` failed: %d", ret);
		return ret;
	}

	/* Empty until the first session with the cloud */
	bool has_cloud = true;

	ret = hio_cloud_get_session_config_hash(&cloud);
	if (ret == -ENODATA) {
		has_cloud = false;
	} else if (ret) {
		LOG_ERR("Call `hio_cloud_get_session_config_hash` failed: %d", ret);
		return ret;
	}

	hio_atci_printfln(atci, "$CONFIGHASH: \"saved\",\"%016llx\"", saved);
	if (has_cloud) {
		hio_atci_printfln(atci, "$CONFIGHASH: \"cloud\",\"%016llx\"", cloud);
	} else {
		hio_atci_printfln(atci, "$CONFIGHASH: \"cloud\",\"\"");
	}

	return 0;
}

HIO_ATCI_CMD_REGISTER(confighash, "$CONFIGHASH", CONFIG_HIO_CLOUD_ATCI_CMD_AUTH_FLAGS, NULL, NULL,
		      at_confighash_read, NULL,
		      "Config hashes: saved (uploaded to the cloud) and known by the cloud.");
