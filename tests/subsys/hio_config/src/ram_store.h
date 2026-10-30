/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef RAM_STORE_H_
#define RAM_STORE_H_

#include <stddef.h>

/* Register a RAM-backed settings store as both the load source and the save
 * destination (SETTINGS_NONE provides neither); repeated calls are no-ops. */
void ram_store_register(void);

/* Write a raw value to the store, bypassing the settings handlers */
void ram_store_put(const char *name, const void *value, size_t len);

/* Remove all values from the store */
void ram_store_clear(void);

#endif /* RAM_STORE_H_ */
