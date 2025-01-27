/* HIO includes */
#include <hio/hio_rtc.h>

/* Zephyr includes */
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/timeutil.h>

#include <nrfx_rtc.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(hio_rtc, CONFIG_HIO_RTC_LOG_LEVEL);

static const nrfx_rtc_t m_rtc = NRFX_RTC_INSTANCE(0);

static struct onoff_client m_lfclk_cli;

static int m_year = 1970;
static int m_month = 1;
static int m_day = 1;
static int m_wday = 4;
static int m_hours = 0;
static int m_minutes = 0;
static int m_seconds = 0;

static int get_days_in_month(int year, int month)
{
	if (month < 1 || month > 12) {
		return 0;
	}

	if (month == 4 || month == 6 || month == 9 || month == 11) {
		return 30;

	} else if (month == 2) {
		if (year % 400 == 0 || (year % 100 != 0 && year % 4 == 0)) {
			return 29;

		} else {
			return 28;
		}
	}

	return 31;
}

static int get_day_of_week(int year, int month, int day)
{
	int adjustment = (14 - month) / 12;
	int m = month + 12 * adjustment - 2;
	int y = year - adjustment;
	int w = (day + (13 * m - 1) / 5 + y + y / 4 - y / 100 + y / 400) % 7;

	return w ? w : 7;
}

static int set_tm(const struct hio_rtc_tm *tm)
{
	if (tm->year < 1970 || tm->year > 2099) {
		return -EINVAL;
	}

	if (tm->month < 1 || tm->month > 12) {
		return -EINVAL;
	}

	if (tm->day < 1 || tm->day > get_days_in_month(tm->year, tm->month)) {
		return -EINVAL;
	}

	if (tm->hours < 0 || tm->hours > 23) {
		return -EINVAL;
	}

	if (tm->minutes < 0 || tm->minutes > 59) {
		return -EINVAL;
	}

	if (tm->seconds < 0 || tm->seconds > 59) {
		return -EINVAL;
	}

	irq_disable(RTC0_IRQn);

	m_year = tm->year;
	m_month = tm->month;
	m_day = tm->day;
	m_wday = get_day_of_week(m_year, m_month, m_day);
	m_hours = tm->hours;
	m_minutes = tm->minutes;
	m_seconds = tm->seconds;

	irq_enable(RTC0_IRQn);

	return 0;
}

int hio_rtc_get_ts(int64_t *ts)
{
	struct tm tm = {0};

	irq_disable(RTC0_IRQn);

	tm.tm_year = m_year - 1900;
	tm.tm_mon = m_month - 1;
	tm.tm_mday = m_day;
	tm.tm_hour = m_hours;
	tm.tm_min = m_minutes;
	tm.tm_sec = m_seconds;
	tm.tm_isdst = -1;

	irq_enable(RTC0_IRQn);

	*ts = timeutil_timegm64(&tm);

	return 0;
}

int hio_rtc_set_ts(int64_t ts)
{
	int ret;

	time_t time = ts;
	struct tm result = {0};
	if (!gmtime_r(&time, &result)) {
		LOG_ERR("Call `gmtime_r` failed");
		return -EINVAL;
	}

	struct hio_rtc_tm tm = {
		.year = result.tm_year + 1900,
		.month = result.tm_mon + 1,
		.day = result.tm_mday,
		.hours = result.tm_hour,
		.minutes = result.tm_min,
		.seconds = result.tm_sec,
	};

	ret = set_tm(&tm);
	if (ret) {
		LOG_ERR("Call `set_tm` failed: %d", ret);
		return ret;
	}

	return 0;
}

static void rtc_handler(nrfx_rtc_int_type_t int_type)
{
	if (int_type != NRFX_RTC_INT_TICK) {
		return;
	}

	static int prescaler = 0;

	if (++prescaler % 8 != 0) {
		return;
	}

	prescaler = 0;

	if (++m_seconds < 60) {
		return;
	}

	m_seconds = 0;

	if (++m_minutes < 60) {
		return;
	}

	m_minutes = 0;

	if (++m_hours < 24) {
		return;
	}

	m_hours = 0;

	if (++m_wday >= 8) {
		m_wday = 1;
	}

	if (++m_day <= get_days_in_month(m_year, m_month)) {
		return;
	}

	m_day = 1;

	if (++m_month > 12) {
		m_month = 1;

		++m_year;
	}
}

static int request_lfclk(void)
{
	int ret;

	struct onoff_manager *mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_LF);
	if (!mgr) {
		LOG_ERR("Call `z_nrf_clock_control_get_onoff` failed");
		return -ENXIO;
	}

	static struct k_poll_signal sig;
	k_poll_signal_init(&sig);
	sys_notify_init_signal(&m_lfclk_cli.notify, &sig);

	ret = onoff_request(mgr, &m_lfclk_cli);
	if (ret < 0) {
		LOG_ERR("Call `onoff_request` failed: %d", ret);
		return ret;
	}

	struct k_poll_event events[] = {
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &sig),
	};
	ret = k_poll(events, ARRAY_SIZE(events), K_FOREVER);
	if (ret) {
		LOG_ERR("Call `k_poll` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

	ret = request_lfclk();
	if (ret) {
		LOG_ERR("Call `request_lfclk` failed: %d", ret);
		return ret;
	}

	nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
	config.prescaler = 4095;
	nrfx_err_t err = nrfx_rtc_init(&m_rtc, &config, rtc_handler);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("Call `nrfx_rtc_init` failed: %d", (int)err);
		return -EIO;
	}

	nrfx_rtc_tick_enable(&m_rtc, true);
	nrfx_rtc_enable(&m_rtc);

	IRQ_CONNECT(RTC0_IRQn, 0, nrfx_rtc_0_irq_handler, NULL, 0);
	irq_enable(RTC0_IRQn);

	return 0;
}

SYS_INIT(init, APPLICATION, 3);
