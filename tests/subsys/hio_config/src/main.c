/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "test_module.h"

#include <zephyr/ztest.h>

static void *suite_setup(void)
{
	zassert_ok(test_module_register(), "test module registration failed");
	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_module_set_defaults();
}

ZTEST_SUITE(hio_config_smoke, NULL, suite_setup, before_each, NULL, NULL);

ZTEST(hio_config_smoke, test_module_registered)
{
	struct hio_config *module;

	zassert_ok(hio_config_find_module("app", &module));
	zassert_equal_ptr(module, &g_test_module);
}
