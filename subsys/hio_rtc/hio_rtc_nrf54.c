/* hio_rtc implementation for the nRF54L/H series (GRTC peripheral).
 *
 * Standalone implementation selected by CMake when CONFIG_SOC_SERIES_NRF54LX is
 * set. Shares only the public API in <hio/hio_rtc.h> with the nRF52/91 variant
 * (hio_rtc_nrf5x.c).
 *
 * Tickless design: wall-clock time is derived from the free-running GRTC
 * SYSCOUNTER (1 MHz, always-on domain, keeps counting through System OFF) plus
 * an anchor {epoch_s, ticks} kept in retained RAM. There is NO per-second
 * interrupt, so the device is never woken just to maintain time.
 *
 * The GRTC SYSCOUNTER only resets on POR / power loss, which is exactly when
 * retained RAM also loses its content. Therefore "retention valid" implies the
 * counter is continuous and the anchor is usable; otherwise time defaults to
 * the epoch and re-anchors.
 */

/* HIO includes */
#include <hio/hio_rtc.h>

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/timeutil.h>

#if DT_HAS_ALIAS(rtc_retention)
#include <zephyr/retention/retention.h>
#endif

/* nrfx includes */
#include <nrfx_grtc.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(hio_rtc, CONFIG_HIO_RTC_LOG_LEVEL);

/* GRTC syscounter runs at 1 MHz. */
#define TICKS_PER_SEC 1000000ULL

/* Bump when the layout/semantics of struct rtc_anchor change, so a stale layout
 * left in retained RAM by an older firmware is rejected. (Retention validity
 * itself is handled by the region prefix/checksum via retention_is_valid().) */
#define RTC_ANCHOR_VERSION 1

struct rtc_anchor {
	uint8_t version;
	int64_t epoch_s; /* wall-clock at the anchor (Unix UTC) */
	uint64_t ticks;  /* syscounter value at the anchor */
};

static struct rtc_anchor m_anchor;
static struct k_spinlock m_lock;

#if DT_HAS_ALIAS(rtc_retention)
static const struct device *retention_rtc = DEVICE_DT_GET(DT_ALIAS(rtc_retention));
#endif

static inline uint64_t now_ticks(void)
{
	return nrfx_grtc_syscounter_get();
}

static void anchor_save(void)
{
#if DT_HAS_ALIAS(rtc_retention)
	struct rtc_anchor snapshot;
	k_spinlock_key_t key = k_spin_lock(&m_lock);

	snapshot = m_anchor;
	k_spin_unlock(&m_lock, key);

	int ret = retention_write(retention_rtc, 0, (uint8_t *)&snapshot, sizeof(snapshot));
	if (ret) {
		LOG_ERR("Call `retention_write` failed: %d", ret);
	}
#endif
}

/* Current wall-clock in seconds, derived from anchor + syscounter delta. */
static int64_t now_epoch(void)
{
	k_spinlock_key_t key = k_spin_lock(&m_lock);
	struct rtc_anchor a = m_anchor;
	k_spin_unlock(&m_lock, key);

	uint64_t now = now_ticks();
	int64_t delta = (now >= a.ticks) ? (int64_t)((now - a.ticks) / TICKS_PER_SEC) : 0;

	return a.epoch_s + delta;
}

/* Re-anchor to the current wall-clock and persist it. Used both on time set and
 * by the periodic checkpoint so retained RAM always holds a fresh wall-clock.
 * The GRTC syscounter resets on a warm reboot (but retained RAM survives), so
 * the checkpoint is what lets time survive reboots without a System-OFF wake. */
static void rtc_reanchor(int64_t epoch_s)
{
	uint64_t now = now_ticks();

	k_spinlock_key_t key = k_spin_lock(&m_lock);
	m_anchor.version = RTC_ANCHOR_VERSION;
	m_anchor.epoch_s = epoch_s;
	m_anchor.ticks = now;
	k_spin_unlock(&m_lock, key);

	anchor_save();
}

/* Periodic wall-clock checkpoint (System ON only; kernel timer does NOT wake
 * from System OFF). Bounds the time lost on a warm reboot to ~one period. */
#define RTC_CHECKPOINT_PERIOD K_SECONDS(1)

static void checkpoint_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_checkpoint_work, checkpoint_work_fn);

static void checkpoint_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	rtc_reanchor(now_epoch());
	k_work_reschedule(&m_checkpoint_work, RTC_CHECKPOINT_PERIOD);
}

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

static void epoch_to_tm(int64_t epoch_s, struct hio_rtc_tm *tm)
{
	time_t t = (time_t)epoch_s;
	struct tm r = {0};

	gmtime_r(&t, &r);

	tm->year = r.tm_year + 1900;
	tm->month = r.tm_mon + 1;
	tm->day = r.tm_mday;
	/* hio convention: Mon=1 .. Sun=7; struct tm: Sun=0 .. Sat=6 */
	tm->wday = (r.tm_wday == 0) ? 7 : r.tm_wday;
	tm->hours = r.tm_hour;
	tm->minutes = r.tm_min;
	tm->seconds = r.tm_sec;
}

int hio_rtc_get_tm(struct hio_rtc_tm *tm)
{
	epoch_to_tm(now_epoch(), tm);

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

	struct tm t = {
		.tm_year = tm->year - 1900,
		.tm_mon = tm->month - 1,
		.tm_mday = tm->day,
		.tm_hour = tm->hours,
		.tm_min = tm->minutes,
		.tm_sec = tm->seconds,
		.tm_isdst = -1,
	};
	rtc_reanchor(timeutil_timegm64(&t));

	return 0;
}

int hio_rtc_get_ts(int64_t *ts)
{
	*ts = now_epoch();

	return 0;
}

int hio_rtc_set_ts(int64_t ts)
{
	rtc_reanchor(ts);

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
	const char *src = "default";
	bool have_time = false;

	LOG_INF("System initialization");

#if DT_NODE_EXISTS(DT_ALIAS(rtc_retention))
	/* Retained RAM survives both System OFF and warm reboot; invalid only after
	 * POR / power loss (which also resets the GRTC syscounter). */
	if (retention_is_valid(retention_rtc) == 1) {
		struct rtc_anchor a;
		int ret = retention_read(retention_rtc, 0, (uint8_t *)&a, sizeof(a));

		if (ret == 0 && a.version == RTC_ANCHOR_VERSION) {
			if (now_ticks() >= a.ticks) {
				/* Syscounter continuous: System OFF wake or no reset.
				 * Anchor + delta gives the exact time (incl. sleep). */
				m_anchor = a;
				src = "restored";
			} else {
				/* Warm reboot: syscounter reset but RAM kept. Restore the
				 * last checkpointed wall-clock and re-anchor to now. Lost
				 * time is bounded by the checkpoint period. */
				rtc_reanchor(a.epoch_s);
				src = "reboot";
			}
			have_time = true;
		}
	}
#endif

	if (!have_time) {
		/* POR / first boot: default to epoch and anchor at the current count. */
		rtc_reanchor(0); /* 1970-01-01T00:00:00Z */
	}

	struct hio_rtc_tm tm;
	hio_rtc_get_tm(&tm);
	LOG_INF("Time (%s): %04d/%02d/%02d %02d:%02d:%02d", src, tm.year, tm.month, tm.day,
		tm.hours, tm.minutes, tm.seconds);

	/* Start the System-ON wall-clock checkpoint (survives warm reboot). */
	k_work_schedule(&m_checkpoint_work, RTC_CHECKPOINT_PERIOD);

	return 0;
}

SYS_INIT(init, APPLICATION, 3);
