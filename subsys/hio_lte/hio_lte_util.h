#ifndef SUBSYS_HIO_LTE_UTIL_H_
#define SUBSYS_HIO_LTE_UTIL_H_

/* HIO includes */
#include <hio/hio_lte.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default receive window when nothing better is known (legacy behavior). */
#define HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC 5
/* NB-IoT fallback when the network granted no usable PSM active time:
 * RRC setup and paging latencies are far higher than on LTE-M. */
#define HIO_LTE_UTIL_RECV_TIMEOUT_NBIOT_SEC 30
/* The receive window covers twice the granted T3324 to absorb the RRC hold
 * and server processing on top of the paging window. */
#define HIO_LTE_UTIL_RECV_TIMEOUT_T3324_MULT 2
/* Upper bound so a generous T3324 grant cannot stall the FSM thread. */
#define HIO_LTE_UTIL_RECV_TIMEOUT_MAX_SEC 60

/**
 * @brief Derive the socket receive timeout from the current registration.
 *
 * Uses the network-granted PSM Active-Time (T3324) when available: the
 * device stays reachable for paging that long after RRC release, so the
 * receive window has to cover it. Falls back to a longer default on NB-IoT
 * and to the legacy default otherwise.
 *
 * @param cereg Decoded +CEREG parameters (may be NULL or invalid).
 *
 * @return Receive timeout in seconds.
 */
int hio_lte_util_recv_timeout_sec(const struct hio_lte_cereg_param *cereg);

/* Hard ceiling for one send's on-air wait. nrf_send with NRF_MSG_WAITACK
 * blocks until the data is on-air, which spans the whole time-to-CSCON-1;
 * field captures on congested China NB-IoT peaked at ~75 s, so 120 s leaves
 * margin above the observed worst case. */
#define HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC 120
/* Floor so a send is always given a usable chance even when the caller's
 * deadline is almost up; sending with a sub-second SNDTIMEO would just fail
 * on-air and waste the radio window. */
#define HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC 5

/**
 * @brief Derive one send's on-air (SNDTIMEO) timeout from the remaining deadline.
 *
 * nrf_send with NRF_MSG_WAITACK blocks until the datagram is on-air; because the
 * modem only attempts a connection (CSCON 1) in response to a send, that wait
 * spans the whole time-to-CSCON-1. The timeout must therefore be long enough to
 * let a slow bearer setup complete, but never longer than the time the caller
 * still has before its overall deadline — otherwise a single send could block
 * past the deadline.
 *
 * @param remaining_sec Seconds left until the caller's deadline, or a negative
 *                      value for "no deadline" (K_FOREVER).
 *
 * @return SNDTIMEO in seconds, clamped to
 *         [HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC, HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC].
 */
int hio_lte_util_send_timeout_sec(int remaining_sec);

/**
 * @brief Merge the networks of one search into the scan result.
 *
 * One entry per PLMN and act: the search in act gives the stat of the
 * networks of act, a network of another act is only added. More than
 * CONFIG_HIO_LTE_SCAN_MAX_ENTRIES are counted, not stored. The status of the
 * networks is the first error.
 *
 * @param scan   Scan result being assembled.
 * @param found  The networks found, the first MIN(count, CONFIG_HIO_LTE_SCAN_MAX_ENTRIES).
 * @param count  Networks the search found.
 * @param act    Access technology the modem searched in (0: either).
 * @param status Result of the search.
 */
void hio_lte_util_scan_merge_networks(struct hio_lte_scan_result *scan,
				      const struct hio_lte_scan_entry *found, size_t count,
				      uint8_t act, int status);

/**
 * @brief The status of the networks of a scan cut short.
 *
 * The first error stays; a search cut short makes it cancelled, a search
 * that did not run partial.
 *
 * @param status  Status of the networks so far.
 * @param running A search was running.
 * @param missed  A wanted search did not run.
 *
 * @return The status of the networks.
 */
int hio_lte_util_scan_cut_status(int status, bool running, bool missed);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_UTIL_H_ */
