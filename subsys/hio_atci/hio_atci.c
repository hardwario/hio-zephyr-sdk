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
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/crc.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(hio_atci, CONFIG_HIO_ATCI_LOG_LEVEL);

#define HIO_ATCI_THREAD_PRIORITY (K_LOWEST_APPLICATION_THREAD_PRIO)
#define ECRC_FORMAT              1001
#define ECRC_MISMATCH            1002

#define EVENT_RX      BIT(0)
#define EVENT_TXDONE  BIT(1)
#define EVENT_KILL    BIT(2)
#define EVENT_LOG_MSG BIT(3)

#define CRC_MODE_DISABLED 0
#define CRC_MODE_ENABLED  1
#define CRC_MODE_OPTIONAL 2

static int default_acl_check_cb(const struct hio_atci *atci, const struct hio_atci_cmd *cmd,
				enum hio_atci_cmd_type type, void *user_data)
{
	if (cmd && cmd->auth_flags == 0) {
		return 0;
	}
	return -EACCES;
}

static hio_atci_auth_check_cb m_auth_check_cb = default_acl_check_cb;
static void *m_auth_user_data = NULL;

BUILD_ASSERT(HIO_ATCI_THREAD_PRIORITY >= K_HIGHEST_APPLICATION_THREAD_PRIO &&
		     HIO_ATCI_THREAD_PRIORITY <= K_LOWEST_APPLICATION_THREAD_PRIO,
	     "Invalid range for thread priority");

static void cmd_buffer_clear(const struct hio_atci *atci)
{
	atci->ctx->cmd_buff[0] = '\0';
	atci->ctx->cmd_buff_len = 0;
}

static void backend_evt_handler(enum hio_atci_backend_evt evt, void *ctx)
{

	switch (evt) {
	case HIO_ATCI_BACKEND_EVT_RX_RDY:
		k_event_post(&((struct hio_atci_ctx *)ctx)->event, EVENT_RX);
		break;
	case HIO_ATCI_BACKEND_EVT_TX_RDY:
		k_event_post(&((struct hio_atci_ctx *)ctx)->event, EVENT_TXDONE);
		break;
	default:
		LOG_ERR("Unknown event %d", evt);
		break;
	}
}

static void pending_on_txdone(const struct hio_atci *atci)
{
	k_event_wait(&atci->ctx->event, EVENT_TXDONE, true, K_FOREVER);
}

void hio_atci_io_write(const struct hio_atci *atci, const void *data, size_t length)
{
	size_t offset = 0;
	size_t tmp_cnt;
	int ret;

	if (atci->ctx->crc_mode) {
		atci->ctx->crc = crc32_ieee_update(atci->ctx->crc, data, length);
	}

	while (length) {
		ret = atci->backend->api->write(atci->backend, &((const uint8_t *)data)[offset],
						length, &tmp_cnt);
		if (length >= tmp_cnt) {
			return;
		}

		offset += tmp_cnt;
		length -= tmp_cnt;
		if (tmp_cnt == 0) {
			pending_on_txdone(atci);
		}
	}
}

void hio_atci_io_endline(const struct hio_atci *atci)
{
	if (atci->ctx->crc_mode) {
		snprintf(atci->ctx->fprintf_buff, CONFIG_HIO_ATCI_PRINTF_BUFF_SIZE, "\t%08X\r\n",
			 atci->ctx->crc);
		hio_atci_io_write(atci, atci->ctx->fprintf_buff,
				  1 + 8 + 2); /* 1 for tab, 8 for CRC, 2 for CRLF */
		atci->ctx->crc = 0;           /* Reset CRC for next command */
	} else {
		hio_atci_io_write(atci, "\r\n", 2);
	}
}

static void fprintf_buffer_flush(const struct hio_atci *atci)
{
	hio_atci_io_write(atci, atci->ctx->fprintf_buff, atci->ctx->fprintf_buff_cnt);
	atci->ctx->fprintf_buff_cnt = 0;
}

static int fprintf_out_func(int c, void *ctx)
{
	const struct hio_atci *atci = (const struct hio_atci *)ctx;

	atci->ctx->fprintf_buff[atci->ctx->fprintf_buff_cnt] = (uint8_t)c;
	atci->ctx->fprintf_buff_cnt++;

	if (atci->ctx->fprintf_buff_cnt == CONFIG_HIO_ATCI_PRINTF_BUFF_SIZE) {
		fprintf_buffer_flush(atci);
	}

	return 0;
}

static void fprintf_fmt(const struct hio_atci *atci, const char *fmt, va_list args)
{
	(void)cbvprintf(fprintf_out_func, (void *)atci, fmt, args);

	fprintf_buffer_flush(atci);
}

void hio_atci_io_writef(const struct hio_atci *atci, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);
}

static int check_crc(const struct hio_atci *atci)
{
	if (atci->ctx->crc_mode == CRC_MODE_DISABLED) {
		return 0;
	}

	/* Test format: AT<TAB><CRC32> */
	if (atci->ctx->cmd_buff_len < 11) {
		if (atci->ctx->crc_mode == CRC_MODE_OPTIONAL) {
			return 0;
		}
		return -ECRC_FORMAT;
	}

	if (atci->ctx->cmd_buff[atci->ctx->cmd_buff_len - 9] != '\t') {
		if (atci->ctx->crc_mode == CRC_MODE_OPTIONAL) {
			return 0;
		}
		return -ECRC_FORMAT;
	}

	uint32_t crc = crc32_ieee_update(0, atci->ctx->cmd_buff, atci->ctx->cmd_buff_len - 9);

	uint32_t cmd_crc = strtoul(&atci->ctx->cmd_buff[atci->ctx->cmd_buff_len - 8], NULL, 16);
	if (crc != cmd_crc) {
		LOG_ERR("CRC mismatch: expected %08X, got %08X", crc, cmd_crc);
		return -ECRC_MISMATCH;
	}

	atci->ctx->cmd_buff_len -= 9; /* Remove tab and CRC */
	atci->ctx->cmd_buff[atci->ctx->cmd_buff_len] = '\0';

	return 0;
}

static int execute(const struct hio_atci *atci)
{

	char *buff = atci->ctx->cmd_buff;

	LOG_INF("cmd: %s len: %d", buff, atci->ctx->cmd_buff_len);

	if (buff[0] != 'A' || buff[1] != 'T') {
		LOG_ERR("Invalid command: %s", buff);
		return -ENOMSG;
	}

	int ret = check_crc(atci);
	if (ret) {
		return ret;
	}

	if (atci->ctx->cmd_buff_len == 2) {
		return 0;
	}

	const struct hio_atci_cmd *cmd = NULL;
	enum hio_atci_cmd_type type = HIO_ATCI_CMD_TYPE_ACTION;
	uint16_t cmd_len = 0;

	for (uint16_t i = 2; i < atci->ctx->cmd_buff_len; i++) {
		if (buff[i] == '=') {
			cmd_len = i - 2;
			if (buff[i + 1] == '?') {
				type = HIO_ATCI_CMD_TYPE_TEST;
			} else {
				type = HIO_ATCI_CMD_TYPE_SET;
			}
			break;
		} else if (buff[i] == '?' && i == atci->ctx->cmd_buff_len - 1) {
			cmd_len = i - 2;
			type = HIO_ATCI_CMD_TYPE_READ;
			break;
		}
	}

	if (cmd_len == 0) {
		cmd_len = atci->ctx->cmd_buff_len - 2;
	}

	STRUCT_SECTION_FOREACH(hio_atci_cmd, item) {
		if (strncmp(item->cmd, buff + 2, cmd_len) == 0 && strlen(item->cmd) == cmd_len) {
			cmd = item;
			break;
		}
	}

	if (!cmd) {
		LOG_ERR("Command not found: %s", buff);
		return -ENOEXEC;
	}

	ret = -ENOTSUP;

	atci->ctx->ret_printed = false;

	LOG_DBG("cmd: %s, type: %d", cmd ? cmd->cmd : "NULL", type);

	if (m_auth_check_cb) {
		ret = m_auth_check_cb(atci, cmd, type, m_auth_user_data);
		if (ret) {
			return ret;
		}
	}

	ret = -ENOTSUP;

	switch (type) {
	case HIO_ATCI_CMD_TYPE_ACTION:
		if (cmd->action) {
			ret = cmd->action(atci);
		}
		break;
	case HIO_ATCI_CMD_TYPE_SET:
		if (cmd->set) {
			ret = cmd->set(atci, buff + cmd_len + 3);
		}
		break;
	case HIO_ATCI_CMD_TYPE_READ:
		if (cmd->read) {
			ret = cmd->read(atci);
		}
		break;
	case HIO_ATCI_CMD_TYPE_TEST:
		if (cmd->test) {
			ret = cmd->test(atci);
		}
		break;
	default:
		break;
	}

	return ret;
}

static void process(const struct hio_atci *atci)
{
	if (atci->ctx->cmd_buff_len < 2) {
		return;
	}

	atci->ctx->crc = 0;

	int ret = execute(atci);

	if (!atci->ctx->ret_printed) {
		if (ret == 0) {
			hio_atci_io_write(atci, "OK", 2);
		} else if (ret == -ENOMSG) {
			hio_atci_io_write(atci, "ERROR: \"Invalid command\"", 24);
		} else if (ret == -ENOEXEC) {
			hio_atci_io_write(atci, "ERROR: \"Command not found\"", 26);
		} else if (ret == -EIO) {
			hio_atci_io_write(atci, "ERROR: \"I/O error\"", 21);
		} else if (ret == -ENOMEM) {
			hio_atci_io_write(atci, "ERROR: \"Out of memory\"", 18);
		} else if (ret == -ENOTSUP) {
			hio_atci_io_write(atci, "ERROR: \"Command not supported\"", 30);
		} else if (ret == -EINVAL) {
			hio_atci_io_write(atci, "ERROR: \"Invalid argument\"", 25);
		} else if (ret == -EACCES) {
			hio_atci_io_write(atci, "ERROR: \"Permission denied\"", 26);
		} else if (ret == -ECRC_FORMAT) {
			hio_atci_io_write(atci, "ERROR: \"Invalid CRC format\"", 27);
		} else if (ret == -ECRC_MISMATCH) {
			hio_atci_io_write(atci, "ERROR: \"CRC mismatch\"", 21);
		} else if (ret < 0) {
			hio_atci_io_writef(atci, "ERROR: \"%d\"", ret);
		}
	}

	hio_atci_io_endline(atci);
}

static void process_ch(const struct hio_atci *atci, char ch)
{
	if (ch == '\x1b') {
		cmd_buffer_clear(atci);
		return;
	}

	if (ch == '\r') {
		return;
	}

	if (ch == '\n') {
		if (atci->ctx->cmd_buff_len > 0) {
			atci->ctx->cmd_buff[atci->ctx->cmd_buff_len] = '\0';
			process(atci);
		}
		cmd_buffer_clear(atci);
		return;
	}

	if (atci->ctx->cmd_buff_len < CONFIG_HIO_ATCI_CMD_BUFF_SIZE - 1) {
		atci->ctx->cmd_buff[atci->ctx->cmd_buff_len++] = ch;
	} else {
		LOG_ERR("ATCI command buffer overflow");
		cmd_buffer_clear(atci);
	}
}

static void process_rx(const struct hio_atci *atci)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	atomic_set(&atci->ctx->processing, 1);

	size_t count = 0;
	char ch;

	while (true) {
		atci->backend->api->read(atci->backend, &ch, 1, &count);
		if (count == 0) {
			return;
		}

		process_ch(atci, ch);
	}

	atomic_set(&atci->ctx->processing, 0);
}

static void atci_thread(void *atci_handle, void *arg_log_backend, void *arg_log_level)
{
	struct hio_atci *atci = (struct hio_atci *)atci_handle;

	LOG_INF("ATCI thread started %s", atci->name);

	if (atci->ctx->state != HIO_ATCI_STATE_INITIALIZED) {
		LOG_ERR("ATCI thread started in invalid state %d", atci->ctx->state);
		return;
	}

	int ret;

	ret = atci->backend->api->enable(atci->backend);
	if (ret) {
		LOG_ERR("ATCI backend enable failed %d", ret);
		return;
	}

	if (atci->log_backend) {
		hio_atci_log_backend_enable(atci->log_backend, atci,
					    (uint32_t)(uintptr_t)arg_log_level);
	}

	atci->ctx->state = HIO_ATCI_STATE_ACTIVE;

	while (true) {
		uint32_t events = k_event_wait(
			&atci->ctx->event, EVENT_RX | EVENT_KILL | EVENT_LOG_MSG, true, K_FOREVER);

		if (events & EVENT_KILL) {
			LOG_INF("ATCI thread killed");
			break;
		}

		k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

		if (events & EVENT_RX) {
			process_rx(atci);
		}

		if (events & EVENT_LOG_MSG) {
			int processed = 0;
			do {
				processed = hio_atci_log_backend_process(atci->log_backend);

				if (atci->ctx->cmd_buff_len) { /* Process pending command */
					k_sleep(K_MSEC(15));
				}
			} while (processed);
		}

		if (atci->backend->api->update) {
			atci->backend->api->update(atci->backend);
		}

		k_mutex_unlock(&atci->ctx->wr_mtx);
	}
}

int hio_atci_init(const struct hio_atci *atci, const void *backend_config, bool log_backend,
		  uint32_t init_log_level)
{
	__ASSERT_NO_MSG(atci);

	if (atci->ctx->tid) {
		return -EALREADY;
	}

	memset(atci->ctx, 0, sizeof(*atci->ctx));

	atci->ctx->crc_mode = CONFIG_HIO_ATCI_CRC_MODE;

	k_event_init(&atci->ctx->event);
	k_mutex_init(&atci->ctx->wr_mtx);

	int ret;
	ret = atci->backend->api->init(atci->backend, backend_config, backend_evt_handler,
				       (void *)atci->ctx);
	if (ret) {
		LOG_ERR("ATCI backend init failed %d", ret);
		return ret;
	}

	atci->ctx->state = HIO_ATCI_STATE_INITIALIZED;

	k_tid_t tid;
	tid = k_thread_create(atci->thread, atci->stack, CONFIG_HIO_ATCI_STACK_SIZE, atci_thread,
			      (void *)atci, (void *)log_backend, UINT_TO_POINTER(init_log_level),
			      HIO_ATCI_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(tid, atci->name);

	atci->ctx->tid = tid;

	return 0;
}

#define OUTPUT_LOCK_BEGIN()                                                                        \
	if (!atci)                                                                                 \
		return -EINVAL;                                                                    \
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {                                           \
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);                      \
		return -EINVAL;                                                                    \
	}                                                                                          \
	bool need_lock = !atomic_get(&atci->ctx->processing);                                      \
	if (need_lock) {                                                                           \
		k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);                                       \
	}

#define OUTPUT_LOCK_END()                                                                          \
	if (need_lock) {                                                                           \
		k_mutex_unlock(&atci->ctx->wr_mtx);                                                \
	}                                                                                          \
	return 0;

int hio_atci_write(const struct hio_atci *atci, const void *data, size_t length)
{

	OUTPUT_LOCK_BEGIN()

	hio_atci_io_write(atci, data, length);

	OUTPUT_LOCK_END()
}

int hio_atci_print(const struct hio_atci *atci, const char *str)
{
	OUTPUT_LOCK_BEGIN()

	hio_atci_io_write(atci, str, strlen(str));

	OUTPUT_LOCK_END()
}

int hio_atci_printf(const struct hio_atci *atci, const char *fmt, ...)
{
	OUTPUT_LOCK_BEGIN()

	va_list args;
	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);

	OUTPUT_LOCK_END()
}

int hio_atci_println(const struct hio_atci *atci, const char *str)
{
	OUTPUT_LOCK_BEGIN()

	hio_atci_io_write(atci, str, strlen(str));
	hio_atci_io_write(atci, "\r\n", 2);

	OUTPUT_LOCK_END()
}

int hio_atci_printfln(const struct hio_atci *atci, const char *fmt, ...)
{
	OUTPUT_LOCK_BEGIN()

	va_list args;
	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);

	hio_atci_io_write(atci, "\r\n", 2);

	OUTPUT_LOCK_END()
}

int hio_atci_error(const struct hio_atci *atci, const char *err)
{
	OUTPUT_LOCK_BEGIN()

	hio_atci_io_write(atci, "ERROR: ", 7);
	hio_atci_io_write(atci, err, strlen(err));
	hio_atci_io_write(atci, "\r\n", 2);

	atci->ctx->ret_printed = true;

	OUTPUT_LOCK_END()
}

int hio_atci_errorf(const struct hio_atci *atci, const char *fmt, ...)
{
	OUTPUT_LOCK_BEGIN()

	hio_atci_io_write(atci, "ERROR: ", 7);

	va_list args;
	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);

	hio_atci_io_write(atci, "\r\n", 2);

	atci->ctx->ret_printed = true;

	OUTPUT_LOCK_END()
}

#define BROADCAST_WAIT_MS 100

int hio_atci_broadcast(const char *str)
{
	STRUCT_SECTION_FOREACH(hio_atci, atci) {
		if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
			continue;
		}

		int timeout = BROADCAST_WAIT_MS;
		while (atomic_get(&atci->ctx->processing)) {
			if (--timeout == 0) {
				break;
			}
			k_sleep(K_MSEC(1));
		}

		if (timeout == 0) {
			LOG_ERR("Timeout for ATCI processing in %s", atci->name);
			continue;
		}

		k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

		hio_atci_io_write(atci, str, strlen(str));
		hio_atci_io_endline(atci);

		k_mutex_unlock(&atci->ctx->wr_mtx);
	}
	return 0;
}

int hio_atci_broadcastf(const char *fmt, ...)
{
	STRUCT_SECTION_FOREACH(hio_atci, atci) {
		if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
			continue;
		}

		int timeout = BROADCAST_WAIT_MS;
		while (atomic_get(&atci->ctx->processing)) {
			if (--timeout == 0) {
				break;
			}
			k_sleep(K_MSEC(1));
		}

		if (timeout == 0) {
			LOG_ERR("Timeout for ATCI processing in %s", atci->name);
			continue;
		}

		k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

		va_list args;
		va_start(args, fmt);
		fprintf_fmt(atci, fmt, args);
		hio_atci_io_endline(atci);
		va_end(args);

		k_mutex_unlock(&atci->ctx->wr_mtx);
	}
	return 0;
}

void hio_atci_get_tmp_buff(const struct hio_atci *atci, char **buff, size_t *len)
{
	if (atci && buff) {
		*buff = atci->ctx->tmp_buff;
	}

	if (atci && len) {
		*len = sizeof(atci->ctx->tmp_buff);
	}
}

void hio_atci_set_auth_check_cb(hio_atci_auth_check_cb cb, void *user_data)
{
	m_auth_check_cb = cb;
	m_auth_user_data = user_data;
}
