#ifndef INCLUDE_BUF_UTIL_H_
#define INCLUDE_BUF_UTIL_H_

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_buf hio_buf
 * @{
 */

#define HIO_BUF_DEFINE(_name, _size)                                                               \
	uint8_t _name##_mem[_size];                                                                \
	struct hio_buf _name = {                                                                   \
		.mem = _name##_mem,                                                                \
		.size = sizeof(_name##_mem),                                                       \
		.len = 0,                                                                          \
	}

#define HIO_BUF_DEFINE_STATIC(_name, _size)                                                        \
	static uint8_t _name##_mem[_size];                                                         \
	static struct hio_buf _name = {                                                            \
		.mem = _name##_mem,                                                                \
		.size = sizeof(_name##_mem),                                                       \
		.len = 0,                                                                          \
	}

struct hio_buf {
	uint8_t *mem;
	size_t size;
	size_t len;
};

int hio_buf_init(struct hio_buf *buf, void *mem, size_t size);
uint8_t *hio_buf_get_mem(struct hio_buf *buf);
size_t hio_buf_get_free(struct hio_buf *buf);
size_t hio_buf_get_used(struct hio_buf *buf);
void hio_buf_reset(struct hio_buf *buf);
void hio_buf_fill(struct hio_buf *buf, int val);
int hio_buf_seek(struct hio_buf *buf, size_t pos);
int hio_buf_append_mem(struct hio_buf *buf, const uint8_t *mem, size_t len);
int hio_buf_append_str(struct hio_buf *buf, const char *str);
int hio_buf_append_char(struct hio_buf *buf, char val);
int hio_buf_append_s8(struct hio_buf *buf, int8_t val);
int hio_buf_append_s16_le(struct hio_buf *buf, int16_t val);
int hio_buf_append_s16_be(struct hio_buf *buf, int16_t val);
int hio_buf_append_s32_le(struct hio_buf *buf, int32_t val);
int hio_buf_append_s32_be(struct hio_buf *buf, int32_t val);
int hio_buf_append_s64_le(struct hio_buf *buf, int64_t val);
int hio_buf_append_s64_be(struct hio_buf *buf, int64_t val);
int hio_buf_append_u8(struct hio_buf *buf, uint8_t val);
int hio_buf_append_u16_le(struct hio_buf *buf, uint16_t val);
int hio_buf_append_u16_be(struct hio_buf *buf, uint16_t val);
int hio_buf_append_u32_le(struct hio_buf *buf, uint32_t val);
int hio_buf_append_u32_be(struct hio_buf *buf, uint32_t val);
int hio_buf_append_u64_le(struct hio_buf *buf, uint64_t val);
int hio_buf_append_u64_be(struct hio_buf *buf, uint64_t val);
int hio_buf_append_float_le(struct hio_buf *buf, float val);
int hio_buf_append_float_be(struct hio_buf *buf, float val);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_BUF_UTIL_H_ */
