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
#include <zcbor_encode.h>

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

/**
 * @brief Send data to the cloud, giving up after @p timeout.
 *
 * @param timeout Deadline for the transfer; K_FOREVER never gives up.
 */
int hio_cloud_send_data(const void *buf, size_t len, k_timeout_t timeout);

/**
 * @brief Send data to the cloud. Equivalent to @ref hio_cloud_send_data with
 *        K_FOREVER.
 *
 * @deprecated Use @ref hio_cloud_send_data with an explicit timeout.
 */
__deprecated int hio_cloud_send(const void *buf, size_t len);

int hio_cloud_get_last_seen_ts(int64_t *ts);

/**
 * @brief Get the hash of the config the cloud gets on the next upload.
 *
 * Only the saved config (what applies after a reboot) is uploaded, never
 * unsaved changes. The result is cached until the config changes (see
 * hio_config_get_gen()), gets saved or a new config module gets registered.
 *
 * @param hash Output hash.
 *
 * @return 0 on success, negative error code on failure.
 */
int hio_cloud_get_config_hash(uint64_t *hash);

/**
 * @brief Get the config hash the cloud knows from the session.
 *
 * Updated when a session starts and after each config upload.
 *
 * @param hash Output hash.
 *
 * @return 0 on success, -ENODATA if there is no session yet.
 */
int hio_cloud_get_session_config_hash(uint64_t *hash);
int hio_cloud_firmware_update(const char *firmwareId);

/**
 * @brief Firmware download progress status.
 *
 * All fields except @ref running are meaningful only while @ref running is true.
 */
struct hio_cloud_dfu_status {
	bool running;    /**< True while a firmware download is in progress. */
	uint32_t offset; /**< Bytes written so far. */
	uint32_t size;   /**< Total image size in bytes. */
	char id[37];     /**< Firmware UUID as string (36 chars + NUL). */
	char target[8];  /**< Firmware target (e.g. "app"). */
	char type[10];   /**< Firmware chunk type (e.g. "chunk", "swap"). */
};

/**
 * @brief Get the firmware download (DFU) status.
 *
 * @p running is true between the first accepted firmware chunk and the reboot
 * that applies the update; false when no update is running or after an aborted
 * one. @p offset and @p size are raw byte counts — the caller computes progress
 * as @c offset*100/size (guarding @c size>0). Lock-free.
 *
 * @param status Output structure.
 * @retval 0       Success.
 * @retval -EINVAL @p status is NULL.
 */
int hio_cloud_get_dfu_status(struct hio_cloud_dfu_status *status);
int hio_cloud_recv(void);

int hio_cloud_cbor_ncellmeas_put(zcbor_state_t *zs, const struct hio_lte_ncellmeas_param *param);

/**
 * @brief Callback encoding the CBOR payload of a data message.
 *
 * Encodes the payload of a data packet (UL_UPLOAD_DATA) with the zcbor encoding
 * state @p zs, which writes directly into the transfer buffer of the cloud subsystem.
 * The packet header is already in place and cannot be overwritten, and the callback
 * cannot write behind the end of the buffer: when the message does not fit, zcbor
 * reports ZCBOR_ERR_NO_PAYLOAD and the message is not sent.
 *
 * The callback must close every container (map, list) it opens.
 *
 * The state has stop_on_error enabled, so the callback does not need to check the
 * result of every put call. The first error is kept and nothing is written after it.
 *
 * The callback runs in the thread of the caller while the cloud subsystem holds its
 * lock, so it must not call any cloud API. It may be called at most once per send,
 * but a send is not guaranteed to call it exactly once, so side effects (for example
 * sequence numbers) should be kept outside of the callback or be idempotent. The
 * message must fit into the transfer buffer minus the 9 byte packet header. The
 * helpers hio_cloud_cbor_free(), hio_cloud_cbor_len(), hio_cloud_cbor_put_raw() and
 * hio_cloud_cbor_put_key_ts() are meant to be used from the callback.
 *
 * @param zs        Encoding state positioned behind the packet header.
 * @param user_data User data passed by the caller of the send function.
 *
 * @retval 0        Success, the message is complete.
 * @retval negative Error (negative errno) which aborts the send and is returned to the
 *                  caller of the send function unchanged. Nothing is sent.
 * @retval positive Not allowed, reserved for the cloud subsystem; treated as -EINVAL.
 */
typedef int (*hio_cloud_data_encode_cb)(zcbor_state_t *zs, void *user_data);

/**
 * @brief Get the number of free bytes left for the payload.
 *
 * Valid only for the state passed to the encode callback.
 *
 * @param zs Encoding state passed to the callback.
 *
 * @return Number of bytes that can still be written.
 */
size_t hio_cloud_cbor_free(const zcbor_state_t *zs);

/**
 * @brief Get the number of payload bytes encoded so far.
 *
 * Valid only for the state passed to the encode callback; the packet header
 * is not counted. Returns 0 outside of an encode callback or for a foreign state.
 *
 * @param zs Encoding state passed to the callback.
 *
 * @return Number of payload bytes written so far.
 */
size_t hio_cloud_cbor_len(const zcbor_state_t *zs);

/**
 * @brief Insert an already encoded CBOR value into the payload.
 *
 * Writes no key. @p cbor must be exactly one complete CBOR data item (a whole map,
 * list, ...); this is not validated. The item is counted in the element count of
 * @p zs like any other encoded element.
 *
 * On lack of space the zcbor state is put into the ZCBOR_ERR_NO_PAYLOAD error,
 * so the failure cannot be missed even if the return value is ignored (the send then
 * fails with -ENOSPC).
 *
 * @param zs   Encoding state passed to the callback.
 * @param cbor Encoded CBOR value.
 * @param len  Length of the encoded value in bytes.
 *
 * @retval 0       Success.
 * @retval -EINVAL  @p zs is NULL, or @p cbor is NULL or @p len is zero. In the latter
 *                  case nothing is written, but the state is put into the
 *                  ZCBOR_ERR_WRONG_VALUE error, so the failure cannot be missed.
 * @retval -ENOSPC  Not enough space (the state is put into the ZCBOR_ERR_NO_PAYLOAD
 *                  error), or the state already holds that error. Nothing is written.
 * @retval -EBADMSG The state already holds any other error. Nothing is written.
 */
int hio_cloud_cbor_put_raw(zcbor_state_t *zs, const void *cbor, size_t len);

/**
 * @brief Encode a key and a timestamp.
 *
 * Writes @p key followed by @p ts as an integer, or by null when @p ts is zero
 * (RTC not set). Errors are reported in the zcbor state.
 *
 * @param zs  Encoding state passed to the callback.
 * @param key Map key.
 * @param ts  Unix timestamp in seconds, 0 for none.
 */
void hio_cloud_cbor_put_key_ts(zcbor_state_t *zs, uint32_t key, int64_t ts);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_H_ */
