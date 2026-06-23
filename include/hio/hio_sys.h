#ifndef HIO_INCLUDE_HIO_SYS_H_
#define HIO_INCLUDE_HIO_SYS_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_sys hio_sys
 * @{
 */

/**
 * @brief Get the cause of the last system reset.
 *
 * @param[out] reset_cause Pointer to store the reset cause.
 * @return 0 on success, negative error code on failure.
 */
int hio_sys_get_reset_cause(uint32_t *reset_cause);

/**
 * @brief Get a string representation of a reset cause flag.
 *
 * @param	flag The reset cause flag.
 * @return      Pointer to the string representation of the reset cause flag.
 */
char *hio_sys_reset_cause_flag_str(uint32_t flag);

/**
 * @brief Reboot the system immediately.
 *
 * Reboot the system in the manner specified by COLD type.
 * When successful, this routine does not return.
 *
 * @param reason Optional string logged before reboot (may be NULL).
 */
void hio_sys_reboot(const char *reason);

/**
 * @brief Reboot the system after a delay.
 *
 * Schedules a cold reboot on the system workqueue after @p delay, so the
 * caller can return and let any in-flight output (e.g. a command response)
 * complete first.
 *
 * @param reason Optional string logged before reboot (may be NULL). Only the
 *               pointer is stored, so it must outlive the delay — pass a
 *               string literal or static storage, not a stack buffer.
 * @param delay  Delay before the reboot is performed.
 */
void hio_sys_reboot_delayed(const char *reason, k_timeout_t delay);

typedef void (*hio_sys_reboot_notifier_cb)(const char *reason, void *user_data);

/**
 * @brief Set the reboot notifier callback.
 *
 * Sets a callback that will be invoked just before the system reboots.
 * This can be used for tasks like flushing buffers or persisting state.
 *
 * Notes:
 *  - Passing NULL removes the callback.
 *  - Only one callback can be set at a time (last call wins).
 *  - The callback cannot prevent the reboot.
 *
 * @param cb        Callback to notify, or NULL to unset.
 * @param user_data User pointer passed back to the callback.
 */
void hio_sys_set_reboot_notifier_cb(hio_sys_reboot_notifier_cb cb, void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_SYS_H_ */
