/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */
#ifndef INCLUDE_HIO_INFO_H_
#define INCLUDE_HIO_INFO_H_

/* Zephyr includes */
#include <zephyr/sys/slist.h>

/* Standard includes */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_info hio_info
 * @{
 */

struct shell;
struct hio_atci;

/**
 * @brief Application hook extending the device information output.
 *
 * Registered hooks are invoked (in registration order) at the end of the
 * `info show` shell command and the `AT$INFO?` read action. Hooks with a
 * non-NULL @a name are also matched by `AT$INFO="name"` and listed by
 * `AT$INFO=?`.
 *
 * The structure must remain valid for the lifetime of the system (typically
 * statically allocated). Register during application initialization, before
 * the shell/ATCI are serviced — registration is not thread-safe.
 */
struct hio_info_hook {
	/** Internal list node (do not initialize). */
	sys_snode_t node;

	/** Key for `AT$INFO="name"` lookup and `AT$INFO=?` listing;
	 *  NULL hides the hook from both. */
	const char *name;

	/** Human-readable label for `AT$INFO=?`; NULL prints as "". */
	const char *label;

	/** Prints the value(s) via shell_print(); NULL to skip shell output. */
	void (*shell)(const struct shell *shell);

	/** Prints the `$INFO:` line(s) via hio_atci_printfln(); NULL to skip
	 *  ATCI output. Return value is propagated for `AT$INFO="name"` and
	 *  ignored during the `AT$INFO?` listing. */
	int (*atci)(const struct hio_atci *atci);
};

int hio_info_hook_register(struct hio_info_hook *hook);

/** @brief Get the list of registered hooks (internal, used by shell/ATCI). */
sys_slist_t *hio_info_hook_list_get(void);

int hio_info_get_vendor_name(const char **vendor_name);
int hio_info_get_product_name(const char **product_name);
int hio_info_get_hw_variant(const char **hw_variant);
int hio_info_get_hw_revision(const char **hw_revision);
int hio_info_get_fw_bundle(const char **fw_bundle);
int hio_info_get_fw_name(const char **fw_name);
int hio_info_get_fw_version(const char **fw_version);
int hio_info_get_serial_number(const char **serial_number);
int hio_info_get_serial_number_uint32(uint32_t *serial_number);
int hio_info_get_product_family(uint8_t *product_family);
int hio_info_get_claim_token(const char **claim_token);
int hio_info_get_ble_devaddr(const char **ble_devaddr);
int hio_info_get_ble_devaddr_uint64(uint64_t *ble_devaddr);
int hio_info_get_ble_passkey(const char **ble_passkey);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_INFO_H_ */
