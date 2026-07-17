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
 * @brief Reboot notifier registration structure.
 *
 * The registrant owns the storage (typically a static variable) and keeps it
 * alive for as long as the notifier is registered, mirroring @ref hio_lte_cb.
 */
struct hio_sys_reboot_notifier {
	sys_snode_t node;                /**< Zephyr singly-linked list support. */
	hio_sys_reboot_notifier_cb cb;   /**< Callback invoked just before reboot. */
	void *user_data;                 /**< User pointer passed back to the callback. */
};

/**
 * @brief Add a reboot notifier.
 *
 * Registers a callback invoked just before the system reboots — useful for
 * flushing buffers or persisting state. Multiple notifiers can be registered;
 * they are invoked in registration order. The callback cannot prevent the
 * reboot.
 *
 * @param notifier Caller-owned notifier (must outlive the registration) with
 *                 @c cb set.
 * @retval 0        Success.
 * @retval -EINVAL  @p notifier or its callback is NULL.
 * @retval -EALREADY Notifier already registered.
 */
int hio_sys_add_reboot_notifier(struct hio_sys_reboot_notifier *notifier);

/**
 * @brief Remove a reboot notifier.
 *
 * @param notifier Previously registered notifier.
 * @retval 0        Success.
 * @retval -EINVAL  @p notifier is NULL.
 * @retval -ENOENT  Notifier not found.
 */
int hio_sys_remove_reboot_notifier(struct hio_sys_reboot_notifier *notifier);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_SYS_H_ */
