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
hio_sys_reboot_notifier_cb m_reboot_notifier_cb = NULL;
void *m_reboot_notifier_user_data = NULL;

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
	if (m_reboot_notifier_cb) {
		LOG_INF("Invoking notifier");
		m_reboot_notifier_cb(reason, m_reboot_notifier_user_data);
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

void hio_sys_set_reboot_notifier_cb(hio_sys_reboot_notifier_cb cb, void *user_data)
{
	m_reboot_notifier_cb = cb;
	m_reboot_notifier_user_data = user_data;
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
