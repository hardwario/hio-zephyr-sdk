/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_atci.h>

/* Zephyr includes */
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/serial/uart_async_rx.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_atci_uart, CONFIG_HIO_ATCI_LOG_LEVEL);

#define DT_DRV_COMPAT hio_atci_uart

#define RX_TIMEOUT 10000

#ifndef CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_COUNT
#define CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_COUNT 0
#endif

#ifndef CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_SIZE
#define CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_SIZE 0
#endif

#define ASYNC_RX_BUF_SIZE                                                                          \
	(CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_COUNT *                                      \
	 (CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_SIZE + UART_ASYNC_RX_BUF_OVERHEAD))

struct hio_atci_uart_async {
	const struct device *dev;
	hio_atci_backend_handler_t handler;
	void *handler_ctx;
	bool enabled;
	struct k_mutex mtx;
	// Async
	struct k_sem tx_sem;
	struct k_sem disable_sem;
	struct uart_async_rx async_rx;
	struct uart_async_rx_config async_rx_config;
	atomic_t pending_rx_req;
	uint8_t rx_data[ASYNC_RX_BUF_SIZE];
	// GPIO + debounce
	struct gpio_dt_spec enable_gpio;
	struct gpio_callback gpio_cb;
	struct k_work_delayable debounce_work;
	bool gpio_prev_state;
	const struct hio_atci_backend *backend;
};

static void async_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
	if (!user_data) {
		LOG_ERR("User data is NULL");
		return;
	}

	struct hio_atci_uart_async *uart = (struct hio_atci_uart_async *)user_data;

	switch (evt->type) {
	case UART_TX_DONE:
		k_sem_give(&uart->tx_sem);
		break;
	case UART_RX_RDY:
		if (evt->data.rx.buf == NULL || evt->data.rx.len == 0) {
			LOG_ERR("RX_RDY with NULL buffer or zero length");
		} else {
			uart_async_rx_on_rdy(&uart->async_rx, evt->data.rx.buf, evt->data.rx.len);
		}
		uart->handler(HIO_ATCI_BACKEND_EVT_RX_RDY, uart->handler_ctx);
		break;
	case UART_RX_BUF_REQUEST: {
		uint8_t *buf = uart_async_rx_buf_req(&uart->async_rx);
		size_t len = uart_async_rx_get_buf_len(&uart->async_rx);

		if (buf) {
			int ret = uart_rx_buf_rsp(dev, buf, len);
			if (ret < 0) {
				uart_async_rx_on_buf_rel(&uart->async_rx, buf);
			}
		} else {
			LOG_ERR("RX_BUF_REQUEST failed to get buffer");
			atomic_inc(&uart->pending_rx_req);
		}
		break;
	}
	case UART_RX_BUF_RELEASED:
		if (evt->data.rx_buf.buf == NULL) {
			LOG_ERR("BUF_RELEASED with NULL buffer");
			return;
		}
		uart_async_rx_on_buf_rel(&uart->async_rx, evt->data.rx_buf.buf);
		break;
	case UART_RX_DISABLED:
		LOG_INF("RX disabled");
		k_sem_give(&uart->disable_sem);
		break;
	default:
		break;
	};
}

static void gpio_update_current(const struct hio_atci_backend *backend)
{
	struct hio_atci_uart_async *uart = (struct hio_atci_uart_async *)backend->ctx;

	if (uart->enable_gpio.port == NULL) {
		return;
	}

	bool current = gpio_pin_get_dt(&uart->enable_gpio);
	if (current != uart->gpio_prev_state) {
		LOG_INF("Backend %s", current ? "ENABLED" : "DISABLED");

		uart->gpio_prev_state = current;

		if (current) {
			backend->api->enable(backend);
		} else {
			backend->api->disable(backend);
		}
	}
}

static void debounce_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hio_atci_uart_async *ctx =
		CONTAINER_OF(dwork, struct hio_atci_uart_async, debounce_work);
	gpio_update_current(ctx->backend);
}

static void gpio_callback_handler(const struct device *port, struct gpio_callback *cb,
				  uint32_t pins)
{
	struct hio_atci_uart_async *ctx = CONTAINER_OF(cb, struct hio_atci_uart_async, gpio_cb);
	k_work_schedule(&ctx->debounce_work, K_MSEC(50));
}

static int enable_gpio_init(struct hio_atci_uart_async *uart)
{
	if (!device_is_ready(uart->enable_gpio.port)) {
		LOG_ERR("Enable GPIO port not ready");
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&uart->enable_gpio, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&uart->enable_gpio, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		return ret;
	}

	gpio_init_callback(&uart->gpio_cb, gpio_callback_handler, BIT(uart->enable_gpio.pin));
	gpio_add_callback(uart->enable_gpio.port, &uart->gpio_cb);

	uart->gpio_prev_state = gpio_pin_get_dt(&uart->enable_gpio);
	k_work_init_delayable(&uart->debounce_work, debounce_work_handler);

	return 0;
}

static int init(const struct hio_atci_backend *backend, const void *config,
		hio_atci_backend_handler_t evt_handler, void *ctx)
{
	struct hio_atci_uart_async *uart = backend->ctx;
	const struct device *dev = (const struct device *)config;

	uart->backend = backend;

	uart->dev = dev;
	uart->handler = evt_handler;
	uart->handler_ctx = ctx;

	uart->async_rx_config = (struct uart_async_rx_config){
		.buffer = uart->rx_data,
		.length = ASYNC_RX_BUF_SIZE,
		.buf_cnt = CONFIG_HIO_ATCI_BACKEND_UART_ASYNC_RX_BUFFER_COUNT,
	};

	k_mutex_init(&uart->mtx);
	k_sem_init(&uart->tx_sem, 0, 1);
	k_sem_init(&uart->disable_sem, 0, 1);

	struct uart_async_rx *async_rx = &uart->async_rx;
	int ret = uart_async_rx_init(async_rx, &uart->async_rx_config);
	if (ret < 0) {
		LOG_ERR("Failed to init async RX: %d", ret);
		return ret;
	}

	if (uart->enable_gpio.port != NULL) {
		int ret = enable_gpio_init(uart);
		if (ret < 0) {
			LOG_ERR("Failed to init enable pin: %d", ret);
			return ret;
		}
	}

	return 0;
}

static int enable(const struct hio_atci_backend *backend)
{
	struct hio_atci_uart_async *uart = backend->ctx;
	struct uart_async_rx *async_rx = &uart->async_rx;

	k_mutex_lock(&uart->mtx, K_FOREVER);

	if (uart->enabled) {
		LOG_WRN("UART backend already enabled");
		k_mutex_unlock(&uart->mtx);
		return 0;
	}

	if (uart->enable_gpio.port != NULL) { /* Because enable is call if atci task started */
		if (!gpio_pin_get_dt(&uart->enable_gpio)) {
			pm_device_action_run(uart->dev, PM_DEVICE_ACTION_SUSPEND);
			k_mutex_unlock(&uart->mtx);
			return 0;
		}
	}

	int ret;

	uart_async_rx_reset(async_rx);

	ret = pm_device_action_run(uart->dev, PM_DEVICE_ACTION_RESUME);
	if (ret < 0) {
		LOG_DBG("Failed to resume UART device: %d", ret);
		// return ret;
	}

	ret = uart_callback_set(uart->dev, async_callback, uart);
	if (ret < 0) {
		LOG_ERR("Failed to set UART callback: %d", ret);
		k_mutex_unlock(&uart->mtx);
		return ret;
	}

	uint8_t *buf = uart_async_rx_buf_req(async_rx);
	ret = uart_rx_enable(uart->dev, buf, uart_async_rx_get_buf_len(async_rx), RX_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to enable RX: %d", ret);
		k_mutex_unlock(&uart->mtx);
		return ret;
	}

	uart->enabled = true;

	LOG_INF("UART backend enabled");
	k_mutex_unlock(&uart->mtx);
	return 0;
}

static int disable(const struct hio_atci_backend *backend)
{
	struct hio_atci_uart_async *uart = backend->ctx;

	k_mutex_lock(&uart->mtx, K_FOREVER);

	if (!uart->enabled) {
		LOG_WRN("UART backend already disabled");
		k_mutex_unlock(&uart->mtx);
		return 0;
	}

	uart->enabled = false;

	int ret = uart_rx_disable(uart->dev);
	if (ret < 0) {
		LOG_WRN("RX disable failed (may already be off): %d", ret);
	}

	ret = k_sem_take(&uart->disable_sem, K_MSEC(1000));
	if (ret < 0) {
		LOG_ERR("Failed to disable RX: %d", ret);
	}

	ret = pm_device_action_run(uart->dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret < 0) {
		LOG_ERR("Failed to suspend UART device: %d", ret);
	}

	LOG_INF("UART backend disabled");
	k_mutex_unlock(&uart->mtx);
	return 0;
}

static int write(const struct hio_atci_backend *backend, const void *data, size_t length,
		 size_t *cnt)
{
	struct hio_atci_uart_async *uart = (struct hio_atci_uart_async *)backend->ctx;

	int ret = 0;

	k_mutex_lock(&uart->mtx, K_FOREVER);

	if (uart->enabled) {
		ret = uart_tx(uart->dev, data, length, SYS_FOREVER_US);
		if (ret < 0) {
			*cnt = 0;
		} else {
			*cnt = length;
			k_sem_take(&uart->tx_sem, K_FOREVER);
		}
	}

	uart->handler(HIO_ATCI_BACKEND_EVT_TX_RDY, uart->handler_ctx);

	k_mutex_unlock(&uart->mtx);

	return ret;
}

static int read(const struct hio_atci_backend *backend, void *data, size_t length, size_t *cnt)
{
	struct hio_atci_uart_async *uart = (struct hio_atci_uart_async *)backend->ctx;
	struct uart_async_rx *async_rx = &uart->async_rx;

	uint8_t *buf;
	size_t blen = uart_async_rx_data_claim(async_rx, &buf, length);

	memcpy(data, buf, blen);

	bool buf_available = uart_async_rx_data_consume(async_rx, blen);
	*cnt = blen;

	if (uart->pending_rx_req && buf_available) {
		uint8_t *buf = uart_async_rx_buf_req(async_rx);
		size_t len = uart_async_rx_get_buf_len(async_rx);
		int ret;

		__ASSERT_NO_MSG(buf != NULL);
		atomic_dec(&uart->pending_rx_req);
		ret = uart_rx_buf_rsp(uart->dev, buf, len);
		/* If it is too late and RX is disabled then re-enable it. */
		if (ret < 0) {
			if (ret == -EACCES) {
				uart->pending_rx_req = 0;
				ret = uart_rx_enable(uart->dev, buf, len, RX_TIMEOUT);
			} else {
				return ret;
			}
		}
	}

	return 0;
}

static const struct hio_atci_backend_api hio_atci_uart_backend_api = {
	.init = init,
	.enable = enable,
	.disable = disable,
	.write = write,
	.read = read,
};

#define HIO_ATCI_UART_DEFINE(inst)                                                                 \
	static struct hio_atci_uart_async hio_atci_uart_async_##inst = {                           \
		.dev = NULL,                                                                       \
		.handler = NULL,                                                                   \
		.handler_ctx = NULL,                                                               \
		.enable_gpio =                                                                     \
			GPIO_DT_SPEC_GET_OR(DT_INST(inst, hio_atci_uart), enable_gpios, {0}),      \
	};                                                                                         \
                                                                                                   \
	static const struct hio_atci_backend hio_atci_uart_backend_##inst = {                      \
		.api = &hio_atci_uart_backend_api,                                                 \
		.ctx = &hio_atci_uart_async_##inst,                                                \
	};                                                                                         \
                                                                                                   \
	const struct device *const dev_uart_##inst =                                               \
		DEVICE_DT_GET(DT_PROP(DT_DRV_INST(inst), uart));                                   \
                                                                                                   \
	HIO_ATCI_DEFINE(hio_atci_uart_##inst, &hio_atci_uart_backend_##inst,                       \
			CONFIG_HIO_ATCI_BACKEND_UART_LOG_QUEUE_SIZE,                               \
			CONFIG_HIO_ATCI_BACKEND_UART_LOG_TIMEOUT);                                 \
                                                                                                   \
	static int uart_init_##inst(const struct device *dev)                                      \
	{                                                                                          \
		LOG_INF("Initializing ATCI UART backend %d", inst);                                \
		if (!device_is_ready(dev_uart_##inst)) {                                           \
			LOG_ERR("UART device for instance %d not ready", inst);                    \
			return -ENODEV;                                                            \
		}                                                                                  \
                                                                                                   \
		bool log_backend = CONFIG_HIO_ATCI_LOG_LEVEL > 0;                                  \
		uint32_t level = (CONFIG_HIO_ATCI_LOG_LEVEL > LOG_LEVEL_DBG)                       \
					 ? CONFIG_LOG_MAX_LEVEL                                    \
					 : CONFIG_HIO_ATCI_LOG_LEVEL;                              \
                                                                                                   \
		hio_atci_init(&hio_atci_uart_##inst, dev_uart_##inst, log_backend, level);         \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, uart_init_##inst, NULL, NULL, NULL, POST_KERNEL,               \
			      CONFIG_HIO_ATCI_BACKEND_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(HIO_ATCI_UART_DEFINE)
