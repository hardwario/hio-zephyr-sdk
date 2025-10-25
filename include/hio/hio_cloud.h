#ifndef HIO_INCLUDE_CLOUD_H_
#define HIO_INCLUDE_CLOUD_H_

/* HIO includes */
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#include <zcbor_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIO_CLOUD_TRANSFER_BUF_SIZE (16 * 1024)

struct hio_cloud_options {
	uint64_t decoder_hash;
	uint64_t encoder_hash;
	const uint8_t *decoder_buf;
	size_t decoder_len;
	const uint8_t *encoder_buf;
	size_t encoder_len;
};

struct hio_cloud_session {
	uint32_t id;
	uint64_t decoder_hash;
	uint64_t encoder_hash;
	uint64_t config_hash;
	int64_t timestamp;
	char device_id[36 + 1];
	char device_name[32 + 1];
};

enum hio_cloud_event {
	HIO_CLOUD_EVENT_CONNECTED,
	HIO_CLOUD_EVENT_RECV,
};

struct hio_cloud_event_data_recv {
	void *buf;
	size_t len;
};

union hio_cloud_event_data {
	struct hio_cloud_event_data_recv recv;
};

typedef void (*hio_cloud_cb)(enum hio_cloud_event event, union hio_cloud_event_data *data,
			     void *param);

int hio_cloud_init(struct hio_cloud_options *options);
int hio_cloud_wait_initialized(k_timeout_t timeout);
int hio_cloud_is_initialized(bool *initialized);
int hio_cloud_set_callback(hio_cloud_cb user_cb, void *user_data);
int hio_cloud_set_poll_interval(k_timeout_t interval);
int hio_cloud_poll_immediately(void);

int hio_cloud_send(const void *buf, size_t len);

int hio_cloud_get_last_seen_ts(int64_t *ts);
int hio_cloud_firmware_update(const char *firmwareId);
int hio_cloud_recv(void);

int hio_cloud_cbor_ncellmeas_put(zcbor_state_t *zs, const struct hio_lte_ncellmeas_param *param);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_H_ */
