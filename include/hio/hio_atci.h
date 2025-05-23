/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#ifndef INCLUDE_HIO_ATCI_H_
#define INCLUDE_HIO_ATCI_H_

#include <zephyr/kernel.h>
#include <zephyr/logging/log_instance.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_atci AT Command Interface (ATCI)
 * @{
 */

#ifndef CONFIG_HIO_ATCI_CMD_BUFF_SIZE
#define CONFIG_HIO_ATCI_CMD_BUFF_SIZE 0
#endif

#ifndef CONFIG_HIO_ATCI_PRINTF_BUFF_SIZE
#define CONFIG_HIO_ATCI_PRINTF_BUFF_SIZE 0
#endif

/** @brief ATCI internal state. */
enum hio_atci_state {
	HIO_ATCI_STATE_UNINITIALIZED = 0, /**< The ATCI instance is not initialized. */
	HIO_ATCI_STATE_INITIALIZED,       /**< The ATCI instance is initialized. */
	HIO_ATCI_STATE_ACTIVE,            /**< The ATCI instance is running. */
};

/** @brief Backend event type. */
enum hio_atci_backend_evt {
	HIO_ATCI_BACKEND_EVT_RX_RDY, /**< Data received and ready to be read. */
	HIO_ATCI_BACKEND_EVT_TX_RDY  /**< Backend is ready for transmission. */
};

/** @brief Event handler function prototype for backend events. */
typedef void (*hio_atci_backend_handler_t)(enum hio_atci_backend_evt evt, void *ctx);

struct hio_atci_backend;

/**
 * @brief Backend interface API.
 *
 * Provides an abstraction layer for various ATCI transport backends.
 */
struct hio_atci_backend_api {
	/**
	 * @brief Initialize the backend.
	 *
	 * @param[in] backend  Pointer to the backend instance.
	 * @param[in] config   Pointer to the backend configuration structure.
	 * @param[in] handler  Event callback function.
	 * @param[in] ctx      User-defined context passed to the callback.
	 *
	 * @retval 0 on success.
	 * @retval Negative error code on failure.
	 */
	int (*init)(const struct hio_atci_backend *backend, const void *config,
		    hio_atci_backend_handler_t handler, void *ctx);

	/**
	 * @brief Enable the backend for transmission.
	 *
	 * This function may reconfigure the backend to operate in blocking TX mode.
	 *
	 * @param[in] backend  Pointer to the backend instance.
	 *
	 * @retval 0 on success.
	 * @retval Negative error code on failure.
	 */
	int (*enable)(const struct hio_atci_backend *backend);

	/**
	 * @brief Disable the backend for transmission.
	 *
	 * This function may reconfigure the backend to operate in non-blocking TX mode.
	 *
	 * @param[in] backend  Pointer to the backend instance.
	 *
	 * @retval 0 on success.
	 * @retval Negative error code on failure.
	 */
	int (*disable)(const struct hio_atci_backend *backend);

	/**
	 * @brief Transmit data through the backend.
	 *
	 * @param[in]  backend  Pointer to the backend instance.
	 * @param[in]  data     Pointer to the data buffer.
	 * @param[in]  length   Length of the data to be sent.
	 * @param[out] cnt      Pointer to the number of bytes sent (can be NULL).
	 *
	 * @retval 0 on success.
	 * @retval Negative error code on failure.
	 */
	int (*write)(const struct hio_atci_backend *backend, const void *data, size_t length,
		     size_t *cnt);

	/**
	 * @brief Receive data from the backend.
	 *
	 * @param[in]  backend  Pointer to the backend instance.
	 * @param[out] data     Pointer to the destination buffer.
	 * @param[in]  length   Maximum number of bytes to read.
	 * @param[out] cnt      Pointer to the number of bytes read (can be NULL).
	 *
	 * @retval 0 on success.
	 * @retval Negative error code on failure.
	 */
	int (*read)(const struct hio_atci_backend *backend, void *data, size_t length, size_t *cnt);

	/**
	 * @brief Backend maintenance function.
	 *
	 * Called periodically from the ATCI thread loop. Intended for long-running
	 * or deferred operations.
	 *
	 * @param[in] backend  Pointer to the backend instance.
	 */
	void (*update)(const struct hio_atci_backend *backend);
};

/** @brief Backend descriptor structure. */
struct hio_atci_backend {
	const struct hio_atci_backend_api *api; /**< Pointer to backend API. */
	void *ctx;                              /**< Backend-specific context. */
};

/** @brief Internal context of an ATCI instance. */
struct hio_atci_ctx {
	enum hio_atci_state state;
	atomic_t log_lost_cnt;
	k_tid_t tid;
	struct k_event event;
	atomic_t processing;
	struct k_mutex wr_mtx;
	uint16_t cmd_buff_len;
	char cmd_buff[CONFIG_HIO_ATCI_CMD_BUFF_SIZE];
	char fprintf_buff[CONFIG_HIO_ATCI_PRINTF_BUFF_SIZE];
	size_t fprintf_buff_cnt;
	bool error_printed;
};

/** @brief ATCI instance definition. */
struct hio_atci {
	const char *name;                               /**< Name of the instance. */
	struct hio_atci_ctx *ctx;                       /**< Internal context. */
	const struct hio_atci_backend *backend;         /**< Backend interface. */
	struct k_thread *thread;                        /**< Processing thread. */
	k_thread_stack_t *stack;                        /**< Thread stack memory. */
	const struct hio_atci_log_backend *log_backend; /**< Optional log backend. */
	LOG_INSTANCE_PTR_DECLARE(log);                  /**< Logging instance. */
};

#define HIO_ATCI_CMD_ACL_FLAGS_NONE 0x00 /**< No access control. */

/** @brief Command handler definition. */
struct hio_atci_cmd {
	const char *cmd;     /**< Command identifier. */
	uint32_t auth_flags; /**< Authorization flags passed to the auth_check callback.. */
	int (*action)(const struct hio_atci *atci);                /**< AT+CMD */
	int (*set)(const struct hio_atci *atci, const char *argv); /**< AT+CMD=<val> */
	int (*read)(const struct hio_atci *atci);                  /**< AT+CMD? */
	int (*help)(const struct hio_atci *atci);                  /**< Help function. */
	const char *hint; /**< Help hint shown in command list. */
};

/**
 * @brief Register a new ATCI command.
 *
 * @param[in] _name       Command symbol name (for linker section).
 * @param[in] _cmd        Command string.
 * @param[in] _auth_flags Authorization flags for the command.
 * @param[in] _action     Function called on AT+CMD.
 * @param[in] _set        Function called on AT+CMD=<val>.
 * @param[in] _read       Function called on AT+CMD?.
 * @param[in] _help       Help function.
 * @param[in] _hint       Hint for command help listing.
 */
#define HIO_ATCI_CMD_REGISTER(_name, _cmd, _auth_flags, _action, _set, _read, _help, _hint)        \
	static const STRUCT_SECTION_ITERABLE(hio_atci_cmd, _name) = {                              \
		.cmd = _cmd,                                                                       \
		.auth_flags = _auth_flags,                                                         \
		.action = _action,                                                                 \
		.set = _set,                                                                       \
		.read = _read,                                                                     \
		.help = _help,                                                                     \
		.hint = _hint,                                                                     \
	}

/* Placeholder macros for future logging backend integration. */
#define HIO_ATCI_LOG_BACKEND_DEFINE(_name, _buffer_size, _log_queue_size, _log_timeout)
#define HIO_ATCI_LOG_BACKEND_PTR(_name) NULL

/**
 * @brief Define an ATCI instance.
 *
 * @param[in] _name           Symbol name of the instance.
 * @param[in] _prompt         Prompt string.
 * @param[in] _backend        Pointer to the backend definition.
 * @param[in] _log_queue_size Size of the logging queue.
 * @param[in] _log_timeout    Timeout for log queue overflow handling.
 * @param[in] _newline        Newline character sequence.
 */
#define HIO_ATCI_DEFINE(_name, _backend, _log_queue_size, _log_timeout)                            \
	static const struct hio_atci _name;                                                        \
	static struct hio_atci_ctx UTIL_CAT(_name, _ctx);                                          \
	static K_KERNEL_STACK_DEFINE(_name##_stack, CONFIG_HIO_ATCI_STACK_SIZE);                   \
	static struct k_thread _name##_thread;                                                     \
	LOG_INSTANCE_REGISTER(hio_atci, _name, CONFIG_HIO_ATCI_LOG_LEVEL);                         \
	HIO_ATCI_LOG_BACKEND_DEFINE(_name, CONFIG_HIO_ATCI_PRINTF_BUFF_SIZE, _log_queue_size,      \
				    _log_timeout);                                                 \
	static const STRUCT_SECTION_ITERABLE(hio_atci, _name) = {                                  \
		.name = STRINGIFY(_name), .ctx = &UTIL_CAT(_name, _ctx), .backend = _backend,      \
				  .thread = &_name##_thread, .stack = _name##_stack,               \
				  .log_backend = HIO_ATCI_LOG_BACKEND_PTR(_name),                  \
				  LOG_INSTANCE_PTR_INIT(log, hio_atci, _name)}

/**
 * @brief Initialize an ATCI instance and its backend.
 *
 * @param[in] atci           Pointer to the ATCI instance.
 * @param[in] backend_config Pointer to the backend configuration structure.
 * @param[in] log_backend    Enable or disable log backend.
 * @param[in] init_log_level Initial log level to set.
 *
 * @retval 0 on success.
 * @retval Negative error code on failure.
 */
int hio_atci_init(const struct hio_atci *atci, const void *backend_config, bool log_backend,
		  uint32_t init_log_level);

/**
 * @brief Write raw data to the ATCI output.
 *
 * @param[in] atci   Pointer to the ATCI instance.
 * @param[in] data   Pointer to the data buffer.
 * @param[in] length Length of the data to be written.
 */
void hio_atci_write(const struct hio_atci *atci, const void *data, size_t length);

/**
 * @brief Print a string to the ATCI output.
 *
 * @param[in] atci Pointer to the ATCI instance.
 * @param[in] str  Pointer to the string to be printed.
 */
void hio_atci_print(const struct hio_atci *atci, const char *str);

/**
 * @brief Print a formatted string to the ATCI output.
 *
 * @param[in] atci Pointer to the ATCI instance.
 * @param[in] fmt  Pointer to the format string.
 * @param[in] ...  Additional arguments for formatting.
 */
void hio_atci_printf(const struct hio_atci *atci, const char *fmt, ...);

/**
 * @brief Print a string followed by a newline to the ATCI output.
 *
 * @param[in] atci Pointer to the ATCI instance.
 * @param[in] str  Pointer to the string to be printed.
 */
void hio_atci_println(const struct hio_atci *atci, const char *str);

/**
 * @brief Print a formatted string followed by a newline to the ATCI output.
 *
 * @param[in] atci Pointer to the ATCI instance.
 * @param[in] fmt  Pointer to the format string.
 * @param[in] ...  Additional arguments for formatting.
 */
void hio_atci_printfln(const struct hio_atci *atci, const char *fmt, ...);

/**
 * @brief Print a standard ERROR response to the ATCI output.
 *
 * @param[in] atci Pointer to the ATCI instance.
 * @param[in] err  Pointer to the string to be printed.
 */
void hio_atci_error(const struct hio_atci *atci, const char *err);

/**
 * @brief Print a standard ERROR response with a formatted message to the ATCI output.
 *
 * @param[in] atci Pointer to the ATCI instance.
 * @param[in] err  Error code to be printed.
 *
 * @param[in] fmt  Pointer to the format string.
 * @param[in] ...  Additional arguments for formatting.
 */
void hio_atci_errorf(const struct hio_atci *atci, const char *fmt, ...);

/**
 * @brief Print a message to all ATCI outputs.
 *
 * Typically used for unsolicited result codes (URCs) or global system messages.
 * The string is written to all initialized and active ATCI instances.
 *
 * @param[in] str Pointer to the string to be printed.
 */
void hio_atci_broadcast(const char *str);

/**
 * @brief Print a formatted message to all ATCI outputs.
 *
 * Behaves like @ref hio_atci_broadcast, but supports printf-style formatting.
 *
 * @param[in] fmt Pointer to the format string.
 * @param[in] ... Additional arguments for formatting.
 */
void hio_atci_broadcastf(const char *fmt, ...);

enum hio_atci_cmd_type {
	HIO_ATCI_CMD_TYPE_ACTION = 0,
	HIO_ATCI_CMD_TYPE_SET,
	HIO_ATCI_CMD_TYPE_READ,
	HIO_ATCI_CMD_TYPE_TEST
};

/** @brief ACL check callback function prototype. */
typedef int (*hio_atci_auth_check_cb)(const struct hio_atci *atci, const struct hio_atci_cmd *cmd,
				      enum hio_atci_cmd_type type, void *user_data);

/**
 * @brief Set the ACL check callback function for all ATCI instances.
 *
 * @param cb
 * @param user_data
 */
void hio_atci_set_auth_check_cb(hio_atci_auth_check_cb cb, void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HIO_ATCI_H_ */
