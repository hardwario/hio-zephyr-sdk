/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Link stubs for hio_config.c externals not exercised by these tests. */

#include <hio/hio_sys.h>

#include <zephyr/sys/__assert.h>

void hio_sys_reboot(const char *reason)
{
	/* Tests never save/reset; rebooting the test binary is a bug. */
	__ASSERT(false, "unexpected reboot: %s", reason);
}
