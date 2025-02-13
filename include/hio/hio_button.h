/*
 * Copyright (c) 2023 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef CHESTER_INCLUDE_HIO_BUTTON_H_
#define CHESTER_INCLUDE_HIO_BUTTON_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_button hio_button
 * @{
 */

enum hio_button_channel {
	HIO_BUTTON_CHANNEL_INT = 0,
	HIO_BUTTON_CHANNEL_EXT = 1,
};

enum hio_button_event {
	HIO_BUTTON_EVENT_CLICK,
	HIO_BUTTON_EVENT_HOLD
};

typedef void (*hio_button_event_cb)(enum hio_button_channel, enum hio_button_event event, int value,
				    void *user_data);

int hio_button_set_event_cb(hio_button_event_cb cb, void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CHESTER_INCLUDE_HIO_BUTTON_H_ */
