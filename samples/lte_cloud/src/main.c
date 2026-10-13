/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_cloud.h>
#include <hio/hio_log.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdint.h>

/* zcbor includes */
#include <zcbor_encode.h>

/* Generated from codec/cbor-decoder.yaml at build time */
#include <app_codec.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define CLOUD_INIT_TIMEOUT K_MINUTES(10)
#define CLOUD_SEND_TIMEOUT K_MINUTES(2)

/* Message layout (keys from app_codec.h):
 *   { CODEC_KEY_E_SYSTEM: { CODEC_KEY_E_SYSTEM__UPTIME: <seconds> } } */
static int encode(zcbor_state_t *zs)
{
	zs->constant_state->stop_on_error = true;

	zcbor_map_start_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);

	zcbor_uint32_put(zs, CODEC_KEY_E_SYSTEM);
	{
		zcbor_map_start_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);

		zcbor_uint32_put(zs, CODEC_KEY_E_SYSTEM__UPTIME);
		zcbor_uint32_put(zs, (uint32_t)(k_uptime_get() / MSEC_PER_SEC));

		zcbor_map_end_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);
	}

	zcbor_map_end_encode(zs, ZCBOR_VALUE_IS_INDEFINITE_LENGTH);

	if (!zcbor_check_error(zs)) {
		LOG_ERR("Encoding failed: %d", zcbor_peek_error(zs));
		return -EFAULT;
	}

	return 0;
}

int main(void)
{
	int ret;

	/* Decoder (codec/cbor-decoder.yaml) and its hash; hio_cloud uploads the
	 * decoder to the cloud when the hash differs from the session. */
	CODEC_CLOUD_OPTIONS_STATIC(cloud_options);

	LOG_INF("Starting application");

	/* hio_cloud brings up LTE (hio_lte_enable) itself. The claim token and
	 * serial number come from hio_info (PIB or "info" config). */
	ret = hio_cloud_init(&cloud_options);
	if (ret) {
		LOG_ERR("Call `hio_cloud_init` failed: %d", ret);
		return ret;
	}

	ret = hio_cloud_wait_initialized(CLOUD_INIT_TIMEOUT);
	if (ret) {
		LOG_ERR("Call `hio_cloud_wait_initialized` failed: %d", ret);
		return ret;
	}

	static uint8_t buf[64];

	ZCBOR_STATE_E(zs, 2, buf, sizeof(buf), 1);

	ret = encode(zs);
	if (ret) {
		LOG_ERR("Call `encode` failed: %d", ret);
		return ret;
	}

	size_t len = zs[0].payload_mut - buf;

	ret = hio_cloud_send_data(buf, len, CLOUD_SEND_TIMEOUT);
	if (ret) {
		LOG_ERR("Call `hio_cloud_send_data` failed: %d", ret);
		return ret;
	}

	LOG_INF("Message sent (%u bytes)", len);

	return 0;
}
