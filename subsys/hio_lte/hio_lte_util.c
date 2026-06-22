#include "hio_lte_util.h"

/* Zephyr includes */
#include <zephyr/sys/util.h>

int hio_lte_util_recv_timeout_sec(const struct hio_lte_cereg_param *cereg)
{
	if (!cereg || !cereg->valid) {
		return HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC;
	}

	if (cereg->active_time > 0) {
		return CLAMP(cereg->active_time * HIO_LTE_UTIL_RECV_TIMEOUT_T3324_MULT,
			     HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC,
			     HIO_LTE_UTIL_RECV_TIMEOUT_MAX_SEC);
	}

	if (cereg->act == HIO_LTE_CEREG_PARAM_ACT_NBIOT) {
		return HIO_LTE_UTIL_RECV_TIMEOUT_NBIOT_SEC;
	}

	return HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC;
}

int hio_lte_util_send_timeout_sec(int remaining_sec)
{
	if (remaining_sec < 0) {
		/* No deadline (K_FOREVER): use the hard ceiling. */
		return HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC;
	}

	return CLAMP(remaining_sec, HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC,
		     HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC);
}
