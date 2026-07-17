/* HIO includes */
#include <hio/hio_sys.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(hio_sys, CONFIG_HIO_SYS_LOG_LEVEL);

uint32_t m_reset_cause = 0;
static sys_slist_t m_reboot_notifier_list = SYS_SLIST_STATIC_INIT(&m_reboot_notifier_list);

int hio_sys_get_reset_cause(uint32_t *reset_cause)
{
	if (reset_cause) {
		*reset_cause = m_reset_cause;
		return 0;
	}
	return -EINVAL;
}

char *hio_sys_reset_cause_flag_str(uint32_t flag)
{
	switch (flag) {
	case RESET_PIN:
		return "PIN";
	case RESET_SOFTWARE:
		return "SOFTWARE";
	case RESET_BROWNOUT:
		return "BROWNOUT";
	case RESET_POR:
		return "POR";
	case RESET_WATCHDOG:
		return "WATCHDOG";
	case RESET_DEBUG:
		return "DEBUG";
	case RESET_SECURITY:
		return "SECURITY";
	case RESET_LOW_POWER_WAKE:
		return "LOW_POWER_WAKE";
	case RESET_CPU_LOCKUP:
		return "CPU_LOCKUP";
	case RESET_PARITY:
		return "PARITY";
	case RESET_PLL:
		return "PLL";
	case RESET_CLOCK:
		return "CLOCK";
	case RESET_HARDWARE:
		return "HARDWARE";
	case RESET_USER:
		return "USER";
	case RESET_TEMPERATURE:
		return "TEMPERATURE";
	case BIT(15):
		return "BOOTLOADER";
	case BIT(16):
		return "FLASH";
	default:
		return "UNKNOWN";
	}
}

static void reboot(const char *reason)
{
	struct hio_sys_reboot_notifier *notifier;
	SYS_SLIST_FOR_EACH_CONTAINER(&m_reboot_notifier_list, notifier, node) {
		LOG_INF("Invoking notifier");
		notifier->cb(reason, notifier->user_data);
	}

	if (reason) {
		LOG_INF("Reason: %s", reason);
	} else {
		LOG_INF("Reason: (null)");
	}

	k_sleep(K_MSEC(500)); /* Wait for log to be flushed */

	sys_reboot(SYS_REBOOT_COLD);
}

inline void hio_sys_reboot(const char *reason)
{
	reboot(reason);
}

/* Only the pointer is stored, so the reason must outlive the delay (see the
 * header: string literal or otherwise static storage, not a stack buffer). */
static const char *m_delayed_reason;

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	reboot(m_delayed_reason);
}

static K_WORK_DELAYABLE_DEFINE(m_reboot_work, reboot_work_handler);

void hio_sys_reboot_delayed(const char *reason, k_timeout_t delay)
{
	m_delayed_reason = reason;
	k_work_schedule(&m_reboot_work, delay);
}

int hio_sys_add_reboot_notifier(struct hio_sys_reboot_notifier *notifier)
{
	if (!notifier || !notifier->cb) {
		return -EINVAL;
	}

	struct hio_sys_reboot_notifier *iter;
	SYS_SLIST_FOR_EACH_CONTAINER(&m_reboot_notifier_list, iter, node) {
		if (iter == notifier) {
			return -EALREADY;
		}
	}

	sys_slist_append(&m_reboot_notifier_list, &notifier->node);
	return 0;
}

int hio_sys_remove_reboot_notifier(struct hio_sys_reboot_notifier *notifier)
{
	if (!notifier) {
		return -EINVAL;
	}

	bool removed = sys_slist_find_and_remove(&m_reboot_notifier_list, &notifier->node);
	return removed ? 0 : -ENOENT;
}

static int init(void)
{
	hwinfo_get_reset_cause(&m_reset_cause);
	hwinfo_clear_reset_cause();
	return 0;
}

static int info(void)
{
	LOG_INF("Reset cause: %08X", m_reset_cause);
	for (uint32_t flag = 1; flag != 0; flag <<= 1) {
		if (m_reset_cause & flag) {
			LOG_INF("Reset cause: %s", hio_sys_reset_cause_flag_str(flag));
		}
	}
	return 0;
}

SYS_INIT(init, POST_KERNEL, 99);
SYS_INIT(info, APPLICATION, 0);
