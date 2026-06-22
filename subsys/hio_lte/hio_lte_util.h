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

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_UTIL_H_ */
