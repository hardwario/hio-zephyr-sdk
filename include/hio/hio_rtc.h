#ifndef INCLUDE_HIO_RTC_H_
#define INCLUDE_HIO_RTC_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup msm_rtc msm_rtc
 * @{
 */

struct hio_rtc_tm {
	/* Year in the Anno Domini calendar (e.g. 2022) */
	int year;

	/* Month of the year (range 1-12) */
	int month;

	/* Day of the month (range 1-31) */
	int day;

	/* Day of the week (range 1-7; 1 = Mon) */
	int wday;

	/* Hours since midnight (range 0-23) */
	int hours;

	/* Minutes after the hour (range 0-59) */
	int minutes;

	/* Seconds after the minute (range 0-59) */
	int seconds;
};

int hio_rtc_get_tm(struct hio_rtc_tm *tm);
int hio_rtc_set_tm(const struct hio_rtc_tm *tm);
int hio_rtc_get_ts(int64_t *ts);
int hio_rtc_set_ts(int64_t ts);
int hio_rtc_get_utc_string(char *out_str, size_t out_str_size);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_RTC_H_ */
