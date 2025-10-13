#ifndef HIO_INCLUDE_HIO_LOG_H_
#define HIO_INCLUDE_HIO_LOG_H_

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_log hio_log
 * @{
 */

#define HIO_LOG_SET_CURRENT_MODULE_LEVEL(level)                                                    \
	static int init_hio_log(void)                                                              \
	{                                                                                          \
		log_filter_set(NULL, 0, LOG_CURRENT_MODULE_ID(), level);                           \
		return 0;                                                                          \
	}                                                                                          \
	SYS_INIT(init_hio_log, APPLICATION, CONFIG_HIO_LOG_HELPER_MACRO_INIT_PRIORITY);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_LOG_H_ */
