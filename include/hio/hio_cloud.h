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

/** Size of the cloud transfer buffer, set by CONFIG_HIO_CLOUD_TRANSFER_BUF_SIZE. */
#define HIO_CLOUD_TRANSFER_BUF_SIZE CONFIG_HIO_CLOUD_TRANSFER_BUF_SIZE

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

/**
 * @brief Start the cloud subsystem.
 *
 * Initializes the transport and starts the cloud work queue, which sets up the
 * session in the background (create session, upload of the decoder, the encoder
 * and the config); see hio_cloud_wait_initialized(). The send, receive and
 * poll functions return -EPERM until this function has succeeded.
 *
 * @param options Options; referenced, not copied, so they must stay valid.
 *
 * @retval 0         Success.
 * @retval -EINVAL   @p options is NULL or the claim token is not 16 bytes in hex.
 * @retval -EALREADY Already started.
 * @retval -EPERM    The claim token is not set.
 * @retval <0        Error reading the device info or initializing the transport.
 */
int hio_cloud_init(struct hio_cloud_options *options);

/**
 * @brief Wait until the session is set up.
 *
 * The session is set up once it has been created and the decoder, the encoder
 * and the config have been uploaded. Polling (hio_cloud_set_poll_interval())
 * starts at the same moment.
 *
 * @param timeout How long to wait.
 *
 * @retval 0          The session is set up.
 * @retval -ETIMEDOUT It was not set up within @p timeout.
 */
int hio_cloud_wait_initialized(k_timeout_t timeout);

/**
 * @brief Check without waiting whether the session is set up.
 *
 * @param initialized Output, see hio_cloud_wait_initialized().
 *
 * @retval 0       Success.
 * @retval -EINVAL @p initialized is NULL.
 */
int hio_cloud_is_initialized(bool *initialized);

/**
 * @brief Set the callback for data received from the cloud.
 *
 * The callback gets HIO_CLOUD_EVENT_RECV with the payload of each data downlink.
 * It runs in the thread that processes the downlink (the cloud work queue, or
 * the caller of hio_cloud_recv() or hio_cloud_firmware_update()) with the cloud
 * lock held; the data are valid only during the call.
 *
 * @param user_cb   Callback, NULL for none.
 * @param user_data Passed to the callback.
 *
 * @retval 0 Always.
 */
int hio_cloud_set_callback(hio_cloud_cb user_cb, void *user_data);

/**
 * @brief Set the interval of polling the cloud for downlinks.
 *
 * Each poll runs hio_cloud_recv() on the cloud work queue. The interval applies
 * immediately once the session is set up, or from then on.
 *
 * @param interval Polling interval; K_NO_WAIT stops polling (the default).
 *
 * @retval 0 Always.
 */
int hio_cloud_set_poll_interval(k_timeout_t interval);

/**
 * @brief Poll the cloud for downlinks now, on the cloud work queue.
 *
 * Returns once the poll is queued, not when it is done.
 *
 * @retval 0      The poll is queued (or already was).
 * @retval -EPERM hio_cloud_init() has not completed.
 * @retval <0     The work queue rejected the poll.
 */
int hio_cloud_poll_immediately(void);

/**
 * @brief Send data to the cloud, giving up after @p timeout.
 *
 * Waits for the cloud lock without a limit; @p timeout applies to the transfer
 * only. A downlink announced by the cloud is fetched later by the cloud work
 * queue, not by this call.
 *
 * @param buf     Payload of the data message.
 * @param len     Length of the payload, at most HIO_CLOUD_TRANSFER_BUF_SIZE
 *                minus the 9 byte packet header.
 * @param timeout Deadline for the transfer; K_FOREVER never gives up.
 *
 * @retval 0        Sent.
 * @retval -EINVAL  @p buf is NULL or @p len is zero.
 * @retval -EPERM   hio_cloud_init() has not completed.
 * @retval -EDEADLK Called from inside an encoder callback.
 * @retval -ENOSPC  The payload does not fit the transfer buffer.
 * @retval <0       Transfer error.
 */
int hio_cloud_send_data(const void *buf, size_t len, k_timeout_t timeout);

/**
 * @brief Send data to the cloud. Equivalent to @ref hio_cloud_send_data with
 *        K_FOREVER.
 *
 * @deprecated Use @ref hio_cloud_send_data with an explicit timeout.
 *
 * @param buf Payload of the data message.
 * @param len Length of the payload.
 *
 * @return See hio_cloud_send_data().
 */
__deprecated int hio_cloud_send(const void *buf, size_t len);

/**
 * @brief Get the time of the last successful exchange with the cloud.
 *
 * @param ts Output, RTC timestamp (see hio_rtc_get_ts()) of the last uplink or
 *           downlink.
 *
 * @retval 0       Success.
 * @retval -EINVAL @p ts is NULL.
 * @retval -EPERM  There is no session yet.
 */
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
 * @retval 0        Success.
 * @retval -EINVAL  @p hash is NULL.
 * @retval -ENODATA There is no session yet.
 */
int hio_cloud_get_session_config_hash(uint64_t *hash);

/**
 * @brief Ask the cloud for a firmware update.
 *
 * Sends the request and processes the downlink the cloud answers with in the
 * calling thread; the download itself is driven by the cloud with further
 * downlinks (see hio_cloud_get_dfu_status()). Waits for the cloud lock and the
 * transfer without a limit.
 *
 * @param firmwareId Firmware to download, 1 to 200 characters.
 *
 * @retval 0        The request was sent.
 * @retval -EINVAL  @p firmwareId is empty or longer than 200 characters.
 * @retval -EPERM   hio_cloud_init() has not completed.
 * @retval -EDEADLK Called from inside an encoder callback.
 * @retval <0       Packing or transfer error.
 */
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

/**
 * @brief Fetch a downlink waiting in the cloud and process it.
 *
 * Runs in the calling thread: a received config, shell commands or firmware are
 * processed here and data are passed to the callback (hio_cloud_set_callback()).
 * Further waiting downlinks are fetched by the cloud work queue. Waits for the
 * cloud lock and the transfer without a limit.
 *
 * @retval 0        Success, also when nothing was waiting.
 * @retval -EPERM   hio_cloud_init() has not completed.
 * @retval -EDEADLK Called from inside an encoder callback.
 * @retval <0       Transfer or processing error.
 */
int hio_cloud_recv(void);

/**
 * @brief Encode the result of a neighbor cell measurement.
 *
 * Encodes a list (version, access technology, cells with their neighbors) or
 * null when @p param is not valid. Encoding errors are reported in the zcbor
 * state.
 *
 * @param zs    Encoding state.
 * @param param Measurement result.
 *
 * @retval 0       Success.
 * @retval -EINVAL @p zs or @p param is NULL.
 */
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
 * The callback runs in the caller's thread with the cloud lock held. Calling
 * hio_cloud_send_data(), hio_cloud_send(), hio_cloud_send_data_cb(),
 * hio_cloud_recv() or hio_cloud_firmware_update() from it fails with -EDEADLK;
 * other blocking cloud calls (e.g. hio_cloud_wait_initialized()) may deadlock.
 * Do not take a lock whose holder may call the cloud API.
 *
 * It may be called at most once per send, but a send is not guaranteed to call it
 * exactly once, so side effects (for example sequence numbers) should be kept outside
 * of the callback or be idempotent. The message must fit into the transfer buffer
 * minus the 9 byte packet header. The helpers hio_cloud_cbor_free(),
 * hio_cloud_cbor_len(), hio_cloud_cbor_put_raw() and hio_cloud_cbor_put_key_ts() are
 * meant to be used from the callback.
 *
 * Return 0 on success or a negative errno. Avoid -EAGAIN, -ETIMEDOUT, -EDEADLK
 * and -EPERM: the value is passed through unchanged and cannot be told apart
 * from the errors of hio_cloud_send_data_cb() itself.
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
 * @brief Encode a data message with a callback and send it to the cloud.
 *
 * Waits for the cloud lock, then lets @p cb encode the CBOR payload directly
 * into the cloud transfer buffer behind the data packet header, so the caller
 * needs no message buffer of its own. The callback is called only once the
 * transfer buffer is free, so the data reflect the moment of sending.
 *
 * @p timeout is one deadline for waiting for the cloud lock and for the
 * transfer: the time left after the lock is acquired and the payload is
 * encoded is passed to the transfer layer, which applies it to each packet
 * exchange, so a fragmented message can take longer than @p timeout. If the
 * deadline expires while waiting for the lock, the callback is not called
 * (-EAGAIN); if it has expired before the transfer can start, nothing is sent
 * (-ETIMEDOUT); K_NO_WAIT therefore never sends.
 *
 * If encoding fails, nothing is sent. Test the result with ret != 0;
 * positive values are reserved.
 *
 * @param cb        Encoder callback, see hio_cloud_data_encode_cb.
 * @param user_data Passed to @p cb.
 * @param timeout   Deadline for the lock wait; the rest is the transfer timeout.
 *
 * @retval 0          Sent.
 * @retval -EINVAL    @p cb is NULL, the callback returned a positive value or
 *                    encoded an empty payload.
 * @retval -EPERM     hio_cloud_init() has not completed.
 * @retval -EAGAIN    The deadline expired while waiting for the cloud lock;
 *                    the callback was not called.
 * @retval -ETIMEDOUT The deadline expired before the transfer could start, or
 *                    the transfer timed out.
 * @retval -EDEADLK   Called from inside an encoder callback.
 * @retval -ENOSPC    The payload does not fit the transfer buffer.
 * @retval -ENOMEM    Not enough zcbor backup states (raise
 *                    CONFIG_HIO_CLOUD_ENCODE_ZCBOR_STATES).
 * @retval -EBADMSG   The payload is not valid CBOR.
 * @retval <0         A negative error returned by @p cb, or another transfer
 *                    error.
 */
int hio_cloud_send_data_cb(hio_cloud_data_encode_cb cb, void *user_data, k_timeout_t timeout);

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
