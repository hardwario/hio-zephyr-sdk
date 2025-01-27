#ifndef HIO_INCLUDE_CTR_UTIL_H_
#define HIO_INCLUDE_CTR_UTIL_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_util hio_util
 * @{
 */

int hio_buf2hex(const void *src, size_t src_size, char *dst, size_t dst_size, bool upper);
int hio_hex2buf(const char *src, void *dst, size_t dst_size, bool allow_spaces);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CTR_UTIL_H_ */
