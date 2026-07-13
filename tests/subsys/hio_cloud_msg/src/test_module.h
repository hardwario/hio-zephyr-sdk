/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef TEST_MODULE_H_
#define TEST_MODULE_H_

#include <hio/hio_config.h>

enum test_mode {
	TEST_MODE_OFF = 0,
	TEST_MODE_SLOW = 1,
	TEST_MODE_FAST = 2,
};

struct test_config {
	int interval;
	float threshold;
	bool enabled;
	enum test_mode mode;
	char apn[32];
	uint8_t key[8];
	char secret[16];
	int rovalue;
};

extern struct test_config g_test_config_interim;
extern struct hio_config g_test_module;

int test_module_register(void);
void test_module_set_defaults(void);

#endif /* TEST_MODULE_H_ */
