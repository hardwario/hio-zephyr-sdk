/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_atci, CONFIG_HIO_ATCI_LOG_LEVEL);

#define HIO_ATCI_THREAD_PRIORITY (K_LOWEST_APPLICATION_THREAD_PRIO)

#define EVENT_RX     BIT(0)
#define EVENT_TXDONE BIT(1)
#define EVENT_KILL   BIT(2)

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

static void write(const struct hio_atci *atci, const void *data, size_t length)
{
	size_t offset = 0;
	size_t tmp_cnt;
	int ret;

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

static void fprintf_buffer_flush(const struct hio_atci *atci)
{
	write(atci, atci->ctx->fprintf_buff, atci->ctx->fprintf_buff_cnt);
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

static void writef(const struct hio_atci *atci, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);
}

static void execute(const struct hio_atci *atci)
{
	if (atci->ctx->cmd_buff_len < 2) {
		return;
	}

	char *buff = atci->ctx->cmd_buff;

	LOG_INF("cmd: %s len: %d", buff, atci->ctx->cmd_buff_len);

	if (buff[0] != 'A' || buff[1] != 'T') {
		LOG_ERR("Invalid command: %s", buff);
		write(atci, "ERROR: \"Invalid command\"\r\n", 26);
		return;
	}

	if (atci->ctx->cmd_buff_len == 2) {
		write(atci, "OK\r\n", 4);
		return;
	}

	uint16_t cmd_len = 0;
	enum hio_atci_cmd_type type = HIO_ATCI_CMD_TYPE_ACTION;

	for (uint16_t i = 2; i < atci->ctx->cmd_buff_len; i++) {
		if (buff[i] == '=') {
			cmd_len = i - 2;
			if (buff[i + 1] == '?') {
				type = HIO_ATCI_CMD_TYPE_TEST;
			} else {
				type = HIO_ATCI_CMD_TYPE_SET;
			}
			break;
		} else if (buff[cmd_len] == '?') {
			cmd_len = i - 2;
			type = HIO_ATCI_CMD_TYPE_READ;
			break;
		}
	}

	if (cmd_len == 0) {
		cmd_len = atci->ctx->cmd_buff_len - 2;
	}

	const struct hio_atci_cmd *cmd = NULL;

	STRUCT_SECTION_FOREACH(hio_atci_cmd, item) {
		if (strncmp(item->cmd, buff + 2, cmd_len) == 0) {
			cmd = item;
			break;
		}
	}

	if (!cmd) {
		LOG_ERR("Command not found: %s", buff);
	}

	int ret = -ENOTSUP;
	atci->ctx->error_printed = false;

	LOG_DBG("cmd: %s, type: %d", cmd ? cmd->cmd : "NULL", type);

	if (cmd && m_auth_check_cb) {
		ret = m_auth_check_cb(atci, cmd, type, m_auth_user_data);
		if (ret) {
			cmd = NULL;
		}
	}

	if (cmd) {
		k_mutex_unlock(&atci->ctx->wr_mtx);

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
			if (cmd->read) {
				ret = cmd->read(atci);
			}
			break;
		default:
			break;
		}

		k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);
	}

	if (atci->ctx->error_printed) {
		return;
	}

	if (ret == 0) {
		return write(atci, "OK\r\n", 4);
	} else if (ret == -ENOTSUP) {
		write(atci, "ERROR: \"Command not supported\"\r\n", 32);
	} else if (ret == -EINVAL) {
		write(atci, "ERROR: \"Invalid argument\"\r\n", 27);
	} else if (ret == -EACCES) {
		write(atci, "ERROR: \"Permission denied\"\r\n", 28);
	} else if (ret < 0) {
		writef(atci, "ERROR: \"%d\"\r\n", ret);
	}
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
			execute(atci);
			cmd_buffer_clear(atci);
		}
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

	atci->ctx->state = HIO_ATCI_STATE_ACTIVE;

	while (true) {
		uint32_t events =
			k_event_wait(&atci->ctx->event, EVENT_RX | EVENT_KILL, true, K_FOREVER);

		if (events & EVENT_KILL) {
			LOG_INF("ATCI thread killed");
			break;
		}

		k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

		if (events & EVENT_RX) {
			process_rx(atci);
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

void hio_atci_write(const struct hio_atci *atci, const void *data, size_t length)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	write(atci, data, length);

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_print(const struct hio_atci *atci, const char *str)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	write(atci, str, strlen(str));

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_printf(const struct hio_atci *atci, const char *fmt, ...)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	va_list args;
	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_println(const struct hio_atci *atci, const char *str)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	write(atci, str, strlen(str));
	write(atci, "\r\n", 2);

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_printfln(const struct hio_atci *atci, const char *fmt, ...)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	va_list args;
	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);

	write(atci, "\r\n", 2);

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_error(const struct hio_atci *atci, const char *err)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	write(atci, "ERROR: ", 7);
	write(atci, err, strlen(err));
	write(atci, "\r\n", 2);

	atci->ctx->error_printed = true;

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_errorf(const struct hio_atci *atci, const char *fmt, ...)
{
	if (atci->ctx->state != HIO_ATCI_STATE_ACTIVE) {
		LOG_ERR("ATCI thread in invalid state %d", atci->ctx->state);
		return;
	}

	k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

	va_list args;
	va_start(args, fmt);
	fprintf_fmt(atci, fmt, args);
	va_end(args);

	write(atci, "\r\n", 2);

	atci->ctx->error_printed = true;

	k_mutex_unlock(&atci->ctx->wr_mtx);
}

void hio_atci_broadcast(const char *str)
{
	STRUCT_SECTION_FOREACH(hio_atci, atci) {
		if (atci->ctx->state == HIO_ATCI_STATE_ACTIVE) {
			k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

			write(atci, str, strlen(str));
			write(atci, "\r\n", 2);

			k_mutex_unlock(&atci->ctx->wr_mtx);
		}
	}
}

void hio_atci_broadcastf(const char *fmt, ...)
{
	STRUCT_SECTION_FOREACH(hio_atci, atci) {
		if (atci->ctx->state == HIO_ATCI_STATE_ACTIVE) {
			k_mutex_lock(&atci->ctx->wr_mtx, K_FOREVER);

			va_list args;
			va_start(args, fmt);
			fprintf_fmt(atci, fmt, args);
			write(atci, "\r\n", 2);
			va_end(args);

			k_mutex_unlock(&atci->ctx->wr_mtx);
		}
	}
}

void hio_atci_set_auth_check_cb(hio_atci_auth_check_cb cb, void *user_data)
{
	m_auth_check_cb = cb;
	m_auth_user_data = user_data;
}
