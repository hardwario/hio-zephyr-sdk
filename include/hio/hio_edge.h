/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef INCLUDE_HIO_EDGE_H_
#define INCLUDE_HIO_EDGE_H_

/* Zephyr includes */
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_edge hio_edge
 * @{
 */

enum hio_edge_event {
	HIO_EDGE_EVENT_INACTIVE = 0,
	HIO_EDGE_EVENT_ACTIVE = 1,
};

struct hio_edge;

typedef void (*hio_edge_cb_t)(struct hio_edge *edge, enum hio_edge_event event, void *user_data);

struct hio_edge {
	const struct gpio_dt_spec *spec;
	bool start_active;
	hio_edge_cb_t cb;
	void *user_data;
	atomic_t cooldown_time;
	atomic_t active_duration;
	atomic_t inactive_duration;
	struct k_mutex lock;
	struct k_timer cooldown_timer;
	struct k_timer event_timer;
	struct k_work event_active_work;
	struct k_work event_inactive_work;
	struct gpio_callback gpio_cb;
	atomic_t is_debouncing;
	atomic_t is_active;
};

int hio_edge_init(struct hio_edge *edge, const struct gpio_dt_spec *spec, bool start_active);
int hio_edge_get_active(struct hio_edge *edge, bool *is_active);
int hio_edge_set_callback(struct hio_edge *edge, hio_edge_cb_t cb, void *user_data);
int hio_edge_set_cooldown_time(struct hio_edge *edge, int msec);
int hio_edge_set_active_duration(struct hio_edge *edge, int msec);
int hio_edge_set_inactive_duration(struct hio_edge *edge, int msec);
int hio_edge_watch(struct hio_edge *edge);
int hio_edge_unwatch(struct hio_edge *edge);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_EDGE_H_ */
