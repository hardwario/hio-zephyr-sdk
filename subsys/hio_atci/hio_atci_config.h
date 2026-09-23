/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef HIO_ATCI_CONFIG_H_
#define HIO_ATCI_CONFIG_H_

#if defined(CONFIG_HIO_ATCI_LOGIN)
#include "hio_atci_login.h"
#endif

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent ATCI settings (hio_config module "atci") */
struct hio_atci_config {
#if defined(CONFIG_HIO_ATCI_LOG_BACKEND)
	bool log;
#endif
#if defined(CONFIG_HIO_ATCI_LOGIN)
	char passphrase_hash[HIO_ATCI_LOGIN_HASH_SIZE];
#endif
};

extern struct hio_atci_config g_hio_atci_config;

#ifdef __cplusplus
}
#endif

#endif /* HIO_ATCI_CONFIG_H_ */
