#ifndef SUBSYS_HIO_LTE_MODEM_H_
#define SUBSYS_HIO_LTE_MODEM_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_lte_modem hio_lte_modem
 * @brief LTE modem control and data path API.
 *
 * Starts the LTE stack, handles attach, supports data transfer, and exposes
 * modem and network state for diagnostics and monitoring.
 * @{
 */

/**
 * @brief LTE connection parameters (returned by @ref hio_lte_get_conn_param).
 *
 * If @ref valid is false, other fields are undefined.
 */
struct hio_lte_conn_param {
	bool valid; /**< True if values are valid. */
	int result; /**< Connection evaluation result (convert to text with @ref
		       hio_lte_str_coneval_result). */
	int eest;   /**< Energy estimate (vendor-specific). */
	int ecl;    /**< Coverage enhancement level (e.g. 0..2 for NB-IoT). */
	int rsrp;   /**< Reference Signal Received Power (dBm). */
	int rsrq;   /**< Reference Signal Received Quality (dB). */
	int snr;    /**< Signal-to-noise ratio. */
	int plmn;   /**< PLMN code (MCCMNC) as integer. */
	int cid;    /**< Cell ID. */
	int band;   /**< LTE band number. */
	int earfcn; /**< EARFCN (channel). */
};

/**
 * @brief Registration status values from +CEREG.
 */
enum hio_lte_cereg_param_stat {
	HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED = 0,      /**< Not registered, not searching. */
	HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME = 1,     /**< Registered in home network. */
	HIO_LTE_CEREG_PARAM_STAT_SEARCHING = 2,           /**< Searching for network. */
	HIO_LTE_CEREG_PARAM_STAT_REGISTRATION_DENIED = 3, /**< Registration denied. */
	HIO_LTE_CEREG_PARAM_STAT_UNKNOWN = 4,             /**< Unknown status. */
	HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING = 5,  /**< Registered, roaming. */
	HIO_LTE_CEREG_PARAM_STAT_SIM_FAILURE = 90,        /**< SIM failure. */
};

/**
 * @brief Access technology values (CEREG:AcT).
 */
enum hio_lte_cereg_param_act {
	HIO_LTE_CEREG_PARAM_ACT_UNKNOWN = 0, /**< Unknown. */
	HIO_LTE_CEREG_PARAM_ACT_LTE = 7,     /**< LTE. */
	HIO_LTE_CEREG_PARAM_ACT_NBIOT = 9,   /**< NB-IoT. */
};

/**
 * @brief Decoded +CEREG parameters.
 */
struct hio_lte_cereg_param {
	bool valid;                         /**< True if values are valid. */
	enum hio_lte_cereg_param_stat stat; /**< Registration status. */
	char tac[5];                        /**< Tracking Area Code (hex string, 4 chars + '\0'). */
	int cid;                            /**< Cell Identity (E-UTRAN). */
	enum hio_lte_cereg_param_act act;   /**< Access technology. */
	uint8_t cause_type;                 /**< Reject cause type. */
	uint8_t reject_cause;               /**< Reject cause code (3GPP). */
	int active_time;                    /**< Active-Time (seconds), -1 if disabled. */
	int periodic_tau_ext;               /**< Extended periodic TAU (seconds), -1 if disabled. */
};

/**
 * @brief Parameters for sending and optionally receiving data.
 */
struct hio_lte_send_recv_param {
	bool rai;             /**< Use Release Assistance Indication if supported. */
	const void *send_buf; /**< Buffer with data to send. */
	size_t send_len;      /**< Length of data to send. */
	void *recv_buf;       /**< Buffer for received data (NULL if not needed). */
	size_t recv_size;     /**< Size of receive buffer. */
	size_t *recv_len;     /**< Output: number of received bytes (can not be null if is set
			       * recv_buf).
			       */
	k_timeout_t timeout;  /**< Timeout for the operation. */
};

/**
 * @brief Release Assistance Indication (RAI) parameters.
 */
struct hio_lte_rai_param {
	bool valid;  /**< True if values are valid. */
	bool as_rai; /**< AS-RAI enabled. */
	bool cp_rai; /**< CP-RAI enabled. */
	int cell_id; /**< Cell ID at the time of evaluation. */
	int plmn;    /**< PLMN code (MCCMNC). */
};

/**
 * @brief Communication metrics and timing information.
 *
 * Timestamps *_last_ts are uptime values in milliseconds
 * (see @c hio_rtc_get_ts()).
 */
struct hio_lte_metrics {
	uint32_t attach_count;            /**< Number of successful attach. */
	uint32_t attach_fail_count;       /**< Number of failed attach attempts. */
	uint32_t attach_duration_ms;      /**< Total attach duration (ms). */
	int64_t attach_last_ts;           /**< Uptime of last attach (ms). */
	uint32_t attach_last_duration_ms; /**< Duration of last attach (ms). */

	uint32_t uplink_count;  /**< Number of uplink transmissions. */
	uint32_t uplink_bytes;  /**< Total uplink bytes. */
	uint32_t uplink_errors; /**< Uplink error count. */
	int64_t uplink_last_ts; /**< Uptime of last uplink (ms). */

	uint32_t downlink_count;  /**< Number of downlink receptions. */
	uint32_t downlink_bytes;  /**< Total downlink bytes. */
	uint32_t downlink_errors; /**< Downlink error count. */
	int64_t downlink_last_ts; /**< Uptime of last downlink (ms). */

	uint32_t cscon_1_duration_ms;      /**< Total time in RRC Connected (CSCON=1). */
	uint32_t cscon_1_last_duration_ms; /**< Duration of last RRC Connected period. */
};

/**
 * @brief Attach/retry timeout configuration.
 */
struct hio_lte_attach_timeout {
	k_timeout_t retry_delay;    /**< Delay before retrying attach. */
	k_timeout_t attach_timeout; /**< Maximum duration of one attach attempt. */
};

/**
 * @brief Enable LTE modem and data connection.
 *
 * Performs initialization, configuration, and network attach.
 *
 * @retval 0   Success.
 * @retval -ENOTSUP Test mode is enabled.
 */
int hio_lte_enable(void);

/**
 * @brief Disconnect and reconnect LTE link.
 *
 * Useful for recovering from connection issues.
 *
 * @retval 0   Success.
 * @retval -ENOTSUP Test mode is enabled or not enabled.
 * @retval -ENODEV Modem is disabled.
 */
int hio_lte_reconnect(void);

/**
 * @brief Wait for connection to be established.
 *
 * @param timeout  Maximum wait duration.
 * @retval 0       Connected.
 * @retval -ENOTSUP Test mode is enabled.
 * @retval -ETIMEDOUT Timeout expired.
 */
int hio_lte_wait_for_connected(k_timeout_t timeout);

/**
 * @brief Send data and optionally receive a response.
 *
 * @param param  Operation parameters (see @ref hio_lte_send_recv_param).
 * @retval 0     Success (see @p *recv_len for bytes received).
 * @retval <0    Negative error code.
 */
int hio_lte_send_recv(const struct hio_lte_send_recv_param *param);

/* -------- Information getters -------- */

/**
 * @brief Get device IMEI.
 *
 * @param imei  Output: IMEI as integer.
 * @retval 0    Success.
 * @retval <0   Error.
 */
int hio_lte_get_imei(uint64_t *imei);

/**
 * @brief Get IMSI from SIM/USIM.
 *
 * @param imsi  Output: IMSI as integer.
 * @retval 0    Success.
 * @retval <0   Error.
 */
int hio_lte_get_imsi(uint64_t *imsi);

/**
 * @brief Get ICCID from SIM/USIM.
 *
 * @param iccid Output: newly allocated null-terminated string.
 *              Caller is responsible for freeing.
 * @retval 0    Success.
 * @retval <0   Error.
 */
int hio_lte_get_iccid(char **iccid);

/**
 * @brief Get modem firmware version string.
 *
 * @param version Output: newly allocated null-terminated string.
 *                Caller is responsible for freeing.
 * @retval 0      Success.
 * @retval <0     Error.
 */
int hio_lte_get_modem_fw_version(char **version);

/**
 * @brief Get the latest cached LTE connection parameters.
 *
 * @param param  Output structure.
 * @retval 0     Success (see @ref hio_lte_conn_param::valid).
 * @retval <0    Error.
 */
int hio_lte_get_conn_param(struct hio_lte_conn_param *param);

/**
 * @brief Get the latest cached registration parameters (+CEREG).
 *
 * @param param  Output structure.
 * @retval 0     Success (see @ref hio_lte_cereg_param::valid).
 * @retval <0    Error.
 */
int hio_lte_get_cereg_param(struct hio_lte_cereg_param *param);

/**
 * @brief Get communication metrics.
 *
 * @param metrics Output structure.
 * @retval 0      Success.
 * @retval <0     Error.
 */
int hio_lte_get_metrics(struct hio_lte_metrics *metrics);

/**
 * @brief Get current FSM state as text.
 *
 * @param state  Output: pointer to descriptive string (valid for limited time).
 * @retval 0     Success.
 * @retval <0    Error.
 */
int hio_lte_get_fsm_state(const char **state);

/**
 * @brief Check whether the modem is attached to the network.
 *
 * @param attached Output: true if attached.
 * @retval 0       Success.
 * @retval <0      Error.
 */
int hio_lte_is_attached(bool *attached);

/* -------- Callbacks for LTE events -------- */

/**
 * @brief LTE event types.
 */
enum hio_lte_event {
	HIO_LTE_EVENT_CSCON_0 = 0, /**< Connection Status (Idle). */
	HIO_LTE_EVENT_CSCON_1,     /**< Connection Status (Connected). */
};

/**
 * @brief LTE event callback structure.
 *
 * @note Handler is executed from a worker thread depending on driver
 *       implementation; it must be non-blocking and return quickly.
 */
struct hio_lte_cb {
	sys_snode_t node; /**< Zephyr singly-linked list support. */
	void (*handler)(struct hio_lte_cb *cb, enum hio_lte_event event);
	void *user_data; /**< User data owned by the registrant. */
};

/**
 * @brief Add LTE event callback.
 *
 * @retval 0       Success.
 * @retval -EALREADY Callback already registered.
 * @retval -EINVAL  Invalid argument.
 */
int hio_lte_add_callback(struct hio_lte_cb *cb);

/**
 * @brief Remove LTE event callback.
 *
 * @retval 0       Success.
 * @retval -ENOENT Callback not found.
 * @retval -EINVAL Invalid argument.
 */
int hio_lte_remove_callback(struct hio_lte_cb *cb);

/**
 * @brief Get current attach/retry timeouts.
 *
 * @return Copy of current timeout configuration.
 */
struct hio_lte_attach_timeout hio_lte_get_curr_attach_timeout(void);

/* -------- Utility functions -------- */

/** Convert connection evaluation result code to string. */
const char *hio_lte_str_coneval_result(int result);
/** Convert +CEREG registration status to text. */
const char *hio_lte_str_cereg_stat(enum hio_lte_cereg_param_stat stat);
/** Convert +CEREG registration status to human-readable text. */
const char *hio_lte_str_cereg_stat_human(enum hio_lte_cereg_param_stat stat);
/** Convert access technology (AcT) to text. */
const char *hio_lte_str_act(enum hio_lte_cereg_param_act act);

/** @} */ /* end of group hio_lte_modem */

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_MODEM_H_ */
