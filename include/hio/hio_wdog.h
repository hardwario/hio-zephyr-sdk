/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef CHESTER_INCLUDE_HIO_WDOG_H_
#define CHESTER_INCLUDE_HIO_WDOG_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_wdog hio_wdog
 * @{
 */

struct hio_wdog_channel {
	int id;
};

#if defined(CONFIG_HIO_WDOG)

int hio_wdog_set_timeout(int timeout_msec);
int hio_wdog_install(struct hio_wdog_channel *channel);
int hio_wdog_start(void);
int hio_wdog_feed(struct hio_wdog_channel *channel);

#else

static inline int hio_wdog_set_timeout(int timeout_msec)
{
	return 0;
}

static inline int hio_wdog_install(struct hio_wdog_channel *channel)
{
	return 0;
}

static inline int hio_wdog_start(void)
{
	return 0;
}

static inline int hio_wdog_feed(struct hio_wdog_channel *channel)
{
	return 0;
}

#endif /* defined(CONFIG_HIO_WDOG) */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CHESTER_INCLUDE_HIO_WDOG_H_ */
