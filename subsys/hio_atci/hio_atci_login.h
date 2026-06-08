/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_ATCI_LOGIN_H_
#define HIO_ATCI_LOGIN_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-256 hash represented as a hex string (64 characters + terminator) */
#define HIO_ATCI_LOGIN_HASH_SIZE (64 + 1)

struct hio_atci_login_config {
	char login_hash[HIO_ATCI_LOGIN_HASH_SIZE];
};

extern struct hio_atci_login_config g_hio_atci_login_config;

/**
 * @brief Query whether an authenticated ATCI session is active.
 *
 * @return true if logged in, false otherwise.
 */
bool hio_atci_login_is_logged_in(void);

/**
 * @brief End the authenticated ATCI session.
 */
void hio_atci_login_logout(void);

#ifdef __cplusplus
}
#endif

#endif /* HIO_ATCI_LOGIN_H_ */
