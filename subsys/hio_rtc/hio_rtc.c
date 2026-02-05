/* HIO includes */
#include <hio/hio_rtc.h>

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/timeutil.h>

#if DT_HAS_ALIAS(rtc_retention)
#include <zephyr/retention/retention.h>
#endif

#if defined(CONFIG_SOC_SERIES_NRF54LX)
#include <nrfx_grtc.h>
#else
#include <nrfx_rtc.h>
#endif

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(hio_rtc, CONFIG_HIO_RTC_LOG_LEVEL);

#if defined(CONFIG_SOC_SERIES_NRF54LX)
/* NRF54 uses GRTC peripheral */
#define RTC_IRQn        GRTC_0_IRQn
#define RTC_IRQ_HANDLER nrfx_grtc_irq_handler
static uint8_t m_grtc_channel;
#else
/* NRF52 uses RTC peripheral */
#define RTC_IRQn        RTC0_IRQn
#define RTC_IRQ_HANDLER nrfx_rtc_0_irq_handler
static const nrfx_rtc_t m_rtc = NRFX_RTC_INSTANCE(0);
#endif

static struct onoff_client m_lfclk_cli;

#if DT_HAS_ALIAS(rtc_retention)
/* Retention device for persisting RTC across reboots */
static const struct device *retention_rtc = DEVICE_DT_GET(DT_ALIAS(rtc_retention));
#endif

/* RTC time storage - persisted to retention if available */
struct hio_rtc_tm m_tm = {
	.year = 1970,
	.month = 1,
	.day = 1,
	.wday = 4,
	.hours = 0,
	.minutes = 0,
	.seconds = 0,
};

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

int hio_rtc_get_tm(struct hio_rtc_tm *tm)
{
	irq_disable(RTC_IRQn);

	tm->year = m_tm.year;
	tm->month = m_tm.month;
	tm->day = m_tm.day;
	tm->wday = m_tm.wday;
	tm->hours = m_tm.hours;
	tm->minutes = m_tm.minutes;
	tm->seconds = m_tm.seconds;

	irq_enable(RTC_IRQn);

	return 0;
}

int hio_rtc_set_tm(const struct hio_rtc_tm *tm)
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

	irq_disable(RTC_IRQn);

	m_tm.year = tm->year;
	m_tm.month = tm->month;
	m_tm.day = tm->day;
	m_tm.wday = get_day_of_week(m_tm.year, m_tm.month, m_tm.day);
	m_tm.hours = tm->hours;
	m_tm.minutes = tm->minutes;
	m_tm.seconds = tm->seconds;

	irq_enable(RTC_IRQn);

	return 0;
}

int hio_rtc_get_ts(int64_t *ts)
{
	struct tm tm = {0};

	irq_disable(RTC_IRQn);

	tm.tm_year = m_tm.year - 1900;
	tm.tm_mon = m_tm.month - 1;
	tm.tm_mday = m_tm.day;
	tm.tm_hour = m_tm.hours;
	tm.tm_min = m_tm.minutes;
	tm.tm_sec = m_tm.seconds;
	tm.tm_isdst = -1;

	irq_enable(RTC_IRQn);

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

	ret = hio_rtc_set_tm(&tm);
	if (ret) {
		LOG_ERR("Call `set_tm` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_rtc_get_utc_string(char *out_str, size_t out_str_size)
{
	struct hio_rtc_tm tm;
	int ret = hio_rtc_get_tm(&tm);
	if (ret) {
		LOG_ERR("Call `hio_rtc_get_tm` failed: %d", ret);
		return ret;
	}

	if (snprintf(out_str, out_str_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.year, tm.month,
		     tm.day, tm.hours, tm.minutes, tm.seconds) < 0) {
		LOG_ERR("Call `snprintf` failed");
		return -ENOMEM;
	}

	return 0;
}

#if defined(CONFIG_SOC_SERIES_NRF54LX)
static void rtc_handler(int32_t channel, uint64_t cc_value, void *p_context)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(cc_value);
	ARG_UNUSED(p_context);

	/* Re-arm the timer for next tick (1 second) - use COMPARE reference to avoid drift */
	/* GRTC syscounter runs at 1 MHz, so 1,000,000 ticks = 1 second */
	nrfx_grtc_syscounter_cc_rel_set(m_grtc_channel, 1000000, NRFX_GRTC_CC_RELATIVE_COMPARE);

#else
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
#endif

	if (++m_tm.seconds < 60) {
		goto retention;
	}

	m_tm.seconds = 0;

	if (++m_tm.minutes < 60) {
		goto retention;
	}

	m_tm.minutes = 0;

	if (++m_tm.hours < 24) {
		goto retention;
	}

	m_tm.hours = 0;

	if (++m_tm.wday >= 8) {
		m_tm.wday = 1;
	}

	if (++m_tm.day <= get_days_in_month(m_tm.year, m_tm.month)) {
		goto retention;
	}

	m_tm.day = 1;

	if (++m_tm.month > 12) {
		m_tm.month = 1;

		++m_tm.year;
	}

retention:
#if DT_HAS_ALIAS(rtc_retention)
	/* Save RTC state to retention memory - direct struct copy */
	int ret = retention_write(retention_rtc, 0, (uint8_t *)&m_tm, sizeof(m_tm));
	if (ret) {
		LOG_ERR("Call `retention_write` failed: %d", ret);
	}
#endif
	return;
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

static int cmd_rtc_get(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	struct hio_rtc_tm tm;
	ret = hio_rtc_get_tm(&tm);
	if (ret) {
		LOG_ERR("Call `hio_rtc_get_tm` failed: %d", ret);
		return ret;
	}

	static const char *wday[] = {
		"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
	};

	shell_print(shell, "%04d/%02d/%02d %02d:%02d:%02d %s", tm.year, tm.month, tm.day, tm.hours,
		    tm.minutes, tm.seconds, wday[tm.wday - 1]);

	return 0;
}

static int cmd_rtc_set(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	char *date = argv[1];
	char *time = argv[2];

	/* clang-format off */

	if (strlen(date) != 10 ||
	    !isdigit((unsigned char)date[0]) ||
	    !isdigit((unsigned char)date[1]) ||
	    !isdigit((unsigned char)date[2]) ||
	    !isdigit((unsigned char)date[3]) ||
	    date[4] != '/' ||
	    !isdigit((unsigned char)date[5]) ||
	    !isdigit((unsigned char)date[6]) ||
	    date[7] != '/' ||
	    !isdigit((unsigned char)date[8]) ||
	    !isdigit((unsigned char)date[9])) {
		shell_help(shell);
		return -EINVAL;
	}

	if (strlen(time) != 8 ||
	    !isdigit((unsigned char)time[0]) ||
	    !isdigit((unsigned char)time[1]) ||
	    time[2] != ':' ||
	    !isdigit((unsigned char)time[3]) ||
	    !isdigit((unsigned char)time[4]) ||
	    time[5] != ':' ||
	    !isdigit((unsigned char)time[6]) ||
	    !isdigit((unsigned char)time[7])) {
		shell_help(shell);
		return -EINVAL;
	}

	/* clang-format on */

	date[4] = '\0';
	date[7] = '\0';
	time[2] = '\0';
	time[5] = '\0';

	struct hio_rtc_tm tm;

	tm.year = strtoll(&date[0], NULL, 10);
	tm.month = strtoll(&date[5], NULL, 10);
	tm.day = strtoll(&date[8], NULL, 10);
	tm.hours = strtoll(&time[0], NULL, 10);
	tm.minutes = strtoll(&time[3], NULL, 10);
	tm.seconds = strtoll(&time[6], NULL, 10);

	ret = hio_rtc_set_tm(&tm);
	if (ret) {
		LOG_ERR("Call `hio_rtc_set_tm` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int print_help(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	shell_help(shell);

	return 0;
}

/* clang-format off */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_rtc,

	SHELL_CMD_ARG(get, NULL,
	              "Get current date/time (format YYYY/MM/DD hh:mm:ss).",
	              cmd_rtc_get, 1, 0),

	SHELL_CMD_ARG(set, NULL,
	              "Set current date/time (format YYYY/MM/DD hh:mm:ss).",
	              cmd_rtc_set, 3, 0),

	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(rtc, &sub_rtc, "RTC commands for date/time operations.", print_help);

/* clang-format on */

static int init(void)
{
	int ret;

	LOG_INF("System initialization");

#if DT_HAS_ALIAS(rtc_retention)
	/* Try to restore RTC state from retention memory */
	if (retention_is_valid(retention_rtc)) {
		LOG_INF("Retention valid - restoring RTC state");
		ret = retention_read(retention_rtc, 0, (uint8_t *)&m_tm, sizeof(m_tm));
		if (ret == 0) {
			LOG_INF("Restored: %04d/%02d/%02d %02d:%02d:%02d",
				m_tm.year, m_tm.month, m_tm.day,
				m_tm.hours, m_tm.minutes, m_tm.seconds);
		} else {
			LOG_WRN("Retention read failed: %d", ret);
		}
	} else {
		LOG_INF("Retention not valid - using default time");
	}
#endif

	ret = request_lfclk();
	if (ret) {
		LOG_ERR("Call `request_lfclk` failed: %d", ret);
		return ret;
	}

#if defined(CONFIG_SOC_SERIES_NRF54LX)
	/* NRF54 GRTC initialization */
	ret = nrfx_grtc_init(0);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("Call `nrfx_grtc_init` failed: %d", ret);
		return -EIO;
	}

	if (ret == -EALREADY) {
		LOG_INF("GRTC already initialized");
	}

	/* Allocate GRTC channel */
	ret = nrfx_grtc_channel_alloc(&m_grtc_channel);
	if (ret) {
		LOG_ERR("Call `nrfx_grtc_channel_alloc` failed: %d", ret);
		return -EIO;
	}

	/* Set up callback for the channel */
	nrfx_grtc_channel_callback_set(m_grtc_channel, rtc_handler, NULL);

	/* Enable the IRQ for GRTC */
	IRQ_CONNECT(RTC_IRQn, 0, RTC_IRQ_HANDLER, NULL, 0);
	irq_enable(RTC_IRQn);

	/* Set up initial compare event for 1 second intervals (GRTC runs at 1 MHz) */
	/* Enable interrupt for this channel (third parameter = true) */
	uint64_t compare_value = nrfx_grtc_syscounter_get() + 1000000;
	nrfx_grtc_syscounter_cc_abs_set(m_grtc_channel, compare_value, true);

#else
	/* NRF52 RTC initialization */
	nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
	config.prescaler = 4095;
	nrfx_err_t err = nrfx_rtc_init(&m_rtc, &config, rtc_handler);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("Call `nrfx_rtc_init` failed: %d", (int)err);
		return -EIO;
	}

	nrfx_rtc_tick_enable(&m_rtc, true);
	nrfx_rtc_enable(&m_rtc);

	/* Enable the IRQ for RTC */
	IRQ_CONNECT(RTC_IRQn, 0, RTC_IRQ_HANDLER, NULL, 0);
	irq_enable(RTC_IRQn);
#endif

	return 0;
}

SYS_INIT(init, APPLICATION, 3);
