#ifndef HIO_INCLUDE_HIO_UTIL_H_
#define HIO_INCLUDE_HIO_UTIL_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_util hio_util
 * @{
 */

int hio_buf2hex(const void *src, size_t src_size, char *dst, size_t dst_size, bool upper);
int hio_hex2buf(const char *src, void *dst, size_t dst_size, bool allow_spaces);

/**
 * @brief Format a duration for humans, e.g. "45s", "36m 50s", "1d 02h 03m 04s".
 *
 * @retval 0       Success.
 * @retval -ENOSPC @p dst is too small (24 bytes always fit).
 */
int hio_util_fmt_duration(uint32_t sec, char *dst, size_t dst_size);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_HIO_UTIL_H_ */
