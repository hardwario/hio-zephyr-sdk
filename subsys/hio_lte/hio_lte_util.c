#include "hio_lte_util.h"

/* Zephyr includes */
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <string.h>

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

void hio_lte_util_scan_merge_networks(struct hio_lte_scan_result *scan,
				      const struct hio_lte_scan_entry *found, size_t count,
				      uint8_t act, int status)
{
	size_t stored = MIN(scan->count, ARRAY_SIZE(scan->entries));
	size_t n = MIN(count, ARRAY_SIZE(scan->entries));
	size_t total = scan->count + (count - n);

	for (size_t i = 0; i < n; i++) {
		const struct hio_lte_scan_entry *f = &found[i];
		size_t j = 0;

		while (j < stored &&
		       (scan->entries[j].act != f->act || strcmp(scan->entries[j].plmn, f->plmn))) {
			j++;
		}

		if (j < stored) {
			if (f->act == act) {
				scan->entries[j].stat = f->stat;
			}
			continue;
		}

		if (stored < ARRAY_SIZE(scan->entries)) {
			scan->entries[stored++] = *f;
		}
		total++;
	}

	scan->count = MIN(total, UINT8_MAX);

	if (scan->plmn_status == 0 || scan->plmn_status == -ENODATA) {
		scan->plmn_status = status;
	}
}

int hio_lte_util_scan_cut_status(int status, bool running, bool missed)
{
	if (status != 0 && status != -ENODATA) {
		return status;
	}

	if (running) {
		return -ECANCELED;
	}

	return status == 0 && missed ? -EINTR : status;
}
