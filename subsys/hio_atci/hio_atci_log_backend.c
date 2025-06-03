/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_atci_io.h"

/* HIO includes */
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define EVENT_LOG_MSG BIT(3)

#define STATE_UNINIT   0
#define STATE_ENABLED  1
#define STATE_DISABLED 2
#define STATE_PANIC    3

int hio_atci_log_backend_output_func(uint8_t *data, size_t length, void *ctx)
{
	struct hio_atci *atci = (struct hio_atci *)ctx;
	size_t start = 0;

	for (size_t i = 0; i < length; ++i) {
		uint8_t c = data[i];

		if (c == '"' || c == '\\') {
			/* escape character found, write the data before it */
			if (i > start) {
				if (!atci->ctx->fprintf_flag) {
					hio_atci_io_write(atci, "@LOG: \"", 7);
					atci->ctx->fprintf_flag = 1;
				}
				hio_atci_io_write(atci, &data[start], i - start);
			}

			/* character to escape */
			char esc[2] = {'\\', c};
			hio_atci_io_write(atci, (uint8_t *)esc, 2);

			start = i + 1;
		} else if (c == '\n') {
			/* write data before newline (if any) */
			if (i > start) {
				if (!atci->ctx->fprintf_flag) {
					hio_atci_io_write(atci, "@LOG: \"", 7);
					atci->ctx->fprintf_flag = 1;
				}
				hio_atci_io_write(atci, &data[start], i - start);
			}

			/* end current line and start new one */
			hio_atci_io_write(atci, "\"", 1);
			hio_atci_io_endline(atci);
			atci->ctx->fprintf_flag = 0; /* reset fprintf flag */
			start = i + 1;
		}
	}

	/* write the remaining data */
	if (length > start) {
		if (!atci->ctx->fprintf_flag) {
			hio_atci_io_write(atci, "@LOG: \"", 7);
			atci->ctx->fprintf_flag = 1;
		}

		hio_atci_io_write(atci, &data[start], length - start);
	}

	return (int)length;
}

int hio_atci_log_backend_enable(const struct hio_atci_log_backend *backend,
				const struct hio_atci *atci, uint32_t init_log_level)
{

	if (IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE)) {
		atci->backend->api->enable(atci->backend);
	}

	mpsc_pbuf_init(backend->mpsc_buffer, backend->mpsc_buffer_config); /* fifo reset */
	log_backend_enable(backend->backend, (void *)atci, init_log_level);
	log_output_ctx_set(backend->log_output, (void *)atci);
	backend->ctx->dropped_cnt = 0;
	backend->ctx->state = STATE_ENABLED;
	return 0;
}

int hio_atci_log_backend_disable(const struct hio_atci_log_backend *backend)
{
	log_backend_disable(backend->backend);
	backend->ctx->state = STATE_DISABLED;
	return 0;
}

static void process_log_msg(const struct hio_atci *atci, const struct log_output *log_output,
			    union log_msg_generic *msg, bool locked)
{
	unsigned int key = 0;
	uint32_t flags =
		LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP |
		(IS_ENABLED(CONFIG_HIO_ATCI_LOG_FORMAT_TIMESTAMP) ? LOG_OUTPUT_FLAG_FORMAT_TIMESTAMP
								  : 0) |
		LOG_OUTPUT_FLAG_CRLF_LFONLY;
	if (locked) {
		/*
		 * If running in the thread context, lock the shell mutex to synchronize with
		 * messages printed on the shell thread. In the ISR context, using a mutex is
		 * forbidden so use the IRQ lock to at least synchronize log messages printed
		 * in different contexts.
		 */
		if (k_is_in_isr()) {
			key = irq_lock();
		} else {
			k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);
		}
	}

	log_output_msg_process(log_output, &msg->log, flags);
	if (atci->ctx->fprintf_flag) {
		atci->ctx->fprintf_flag = 0;
		hio_atci_io_write(atci, "\"", 1);
		hio_atci_io_endline(atci);
	}

	if (locked) {
		if (k_is_in_isr()) {
			irq_unlock(key);
		} else {
			k_mutex_unlock(&atci->ctx->wr_mtx);
		}
	}
}

/**
 * @brief Process a log message from the MPSC buffer.
 *
 * @param atci
 * @return int  0 if no message was processed, 1 if a message was processed.
 */
static int process_msg_from_buffer(const struct hio_atci *atci)
{
	const struct hio_atci_log_backend *log_backend = atci->log_backend;
	struct mpsc_pbuf_buffer *mpsc_buffer = log_backend->mpsc_buffer;
	const struct log_output *log_output = log_backend->log_output;
	union log_msg_generic *msg;

	msg = (union log_msg_generic *)mpsc_pbuf_claim(mpsc_buffer);
	if (!msg) {
		return 0;
	}

	process_log_msg(atci, log_output, msg, false);

	mpsc_pbuf_free(mpsc_buffer, &msg->buf);

	return 1;
}

int hio_atci_log_backend_process(const struct hio_atci_log_backend *backend)
{
	const struct hio_atci *atci = (const struct hio_atci *)backend->backend->cb->ctx;
	uint32_t dropped;

	dropped = atomic_set(&backend->ctx->dropped_cnt, 0);
	if (dropped) {

		log_output_dropped_process(backend->log_output, dropped);
	}

	return process_msg_from_buffer(atci);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	const struct hio_atci *atci = (const struct hio_atci *)backend->cb->ctx;
	const struct hio_atci_log_backend *log_backend = atci->log_backend;
	atomic_add(&log_backend->ctx->dropped_cnt, cnt);
}

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	const struct hio_atci *atci = (const struct hio_atci *)backend->cb->ctx;
	const struct hio_atci_log_backend *log_backend = atci->log_backend;
	struct mpsc_pbuf_buffer *mpsc_buffer = log_backend->mpsc_buffer;
	const struct log_output *log_output = log_backend->log_output;

	if (atci->log_backend->ctx->state == STATE_ENABLED) {

		if (IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE)) {
			process_log_msg(atci, log_output, msg, true);
			return;
		}

		size_t wlen = log_msg_generic_get_wlen((union mpsc_pbuf_generic *)msg);
		union mpsc_pbuf_generic *dst =
			mpsc_pbuf_alloc(mpsc_buffer, wlen, K_MSEC(log_backend->timeout));

		/* No space to store the log */
		if (!dst) {
			dropped(backend, 1);
			return;
		}

		uint8_t *dst_data = (uint8_t *)dst + sizeof(struct mpsc_pbuf_hdr);
		uint8_t *src_data = (uint8_t *)msg + sizeof(struct mpsc_pbuf_hdr);
		size_t hdr_wlen = DIV_ROUND_UP(sizeof(struct mpsc_pbuf_hdr), sizeof(uint32_t));
		if (wlen <= hdr_wlen) {
			dropped(backend, 1);
			return;
		}

		dst->hdr.data = msg->buf.hdr.data;
		memcpy(dst_data, src_data, (wlen - hdr_wlen) * sizeof(uint32_t));

		mpsc_pbuf_commit(mpsc_buffer, dst);

		if (IS_ENABLED(CONFIG_MULTITHREADING)) {
			k_event_post(&atci->ctx->event, EVENT_LOG_MSG);
		}

	} else if (atci->log_backend->ctx->state == STATE_PANIC) {
		process_log_msg(atci, log_output, msg, true);
	}
}

static void panic(const struct log_backend *const backend)
{
	const struct hio_atci *atci = (const struct hio_atci *)backend->cb->ctx;
	int err;

	if (IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE)) {
		return;
	}

	err = atci->backend->api->enable(atci->backend);

	if (err == 0) {
		atci->log_backend->ctx->state = STATE_PANIC;

		while (process_msg_from_buffer(atci) > 0) {
			/* empty */
		}
	} else {
		hio_atci_log_backend_disable(atci->log_backend);
	}
}

const struct log_backend_api hio_atci_log_backend_api = {
	.process = process,
	.dropped = dropped,
	.panic = panic,
};
