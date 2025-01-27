/* HIO includes */
#include <hio/hio_buf.h>

/* Zephyr includes */
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int hio_buf_init(struct hio_buf *buf, void *mem, size_t size)
{
	if (!size) {
		return -EINVAL;
	}

	buf->mem = mem;
	buf->size = size;

	hio_buf_reset(buf);

	return 0;
}

uint8_t *hio_buf_get_mem(struct hio_buf *buf)
{
	return buf->mem;
}

size_t hio_buf_get_free(struct hio_buf *buf)
{
	return buf->size - buf->len;
}

size_t hio_buf_get_used(struct hio_buf *buf)
{
	return buf->len;
}

void hio_buf_reset(struct hio_buf *buf)
{
	buf->len = 0;
}

void hio_buf_fill(struct hio_buf *buf, int val)
{
	memset(buf->mem, val, buf->size);

	hio_buf_reset(buf);
}

int hio_buf_seek(struct hio_buf *buf, size_t pos)
{
	if (pos > buf->size) {
		return -EINVAL;
	}

	buf->len = pos;

	return 0;
}

int hio_buf_append_mem(struct hio_buf *buf, const uint8_t *mem, size_t len)
{
	if (hio_buf_get_free(buf) < len) {
		return -ENOSPC;
	}

	memcpy(&buf->mem[buf->len], mem, len);
	buf->len += len;

	return 0;
}

int hio_buf_append_str(struct hio_buf *buf, const char *str)
{
	size_t len = strlen(str);

	if (hio_buf_get_free(buf) < len + 1) {
		return -ENOSPC;
	}

	strcpy(&buf->mem[buf->len], str);
	buf->len += len + 1;

	return 0;
}

int hio_buf_append_char(struct hio_buf *buf, char val)
{
	if (hio_buf_get_free(buf) < 1) {
		return -ENOSPC;
	}

	buf->mem[buf->len] = val;
	buf->len += 1;

	return 0;
}

int hio_buf_append_s8(struct hio_buf *buf, int8_t val)
{
	if (hio_buf_get_free(buf) < 1) {
		return -ENOSPC;
	}

	buf->mem[buf->len] = val;
	buf->len += 1;

	return 0;
}

int hio_buf_append_s16_le(struct hio_buf *buf, int16_t val)
{
	if (hio_buf_get_free(buf) < 2) {
		return -ENOSPC;
	}

	sys_put_le16(val, &buf->mem[buf->len]);
	buf->len += 2;

	return 0;
}

int hio_buf_append_s16_be(struct hio_buf *buf, int16_t val)
{
	if (hio_buf_get_free(buf) < 2) {
		return -ENOSPC;
	}

	sys_put_be16(val, &buf->mem[buf->len]);
	buf->len += 2;

	return 0;
}

int hio_buf_append_s32_le(struct hio_buf *buf, int32_t val)
{
	if (hio_buf_get_free(buf) < 4) {
		return -ENOSPC;
	}

	sys_put_le32(val, &buf->mem[buf->len]);
	buf->len += 4;

	return 0;
}

int hio_buf_append_s32_be(struct hio_buf *buf, int32_t val)
{
	if (hio_buf_get_free(buf) < 4) {
		return -ENOSPC;
	}

	sys_put_be32(val, &buf->mem[buf->len]);
	buf->len += 4;

	return 0;
}

int hio_buf_append_s64_le(struct hio_buf *buf, int64_t val)
{
	if (hio_buf_get_free(buf) < 8) {
		return -ENOSPC;
	}

	sys_put_le64(val, &buf->mem[buf->len]);
	buf->len += 8;

	return 0;
}

int hio_buf_append_s64_be(struct hio_buf *buf, int64_t val)
{
	if (hio_buf_get_free(buf) < 8) {
		return -ENOSPC;
	}

	sys_put_be64(val, &buf->mem[buf->len]);
	buf->len += 8;

	return 0;
}

int hio_buf_append_u8(struct hio_buf *buf, uint8_t val)
{
	if (hio_buf_get_free(buf) < 1) {
		return -ENOSPC;
	}

	buf->mem[buf->len] = val;
	buf->len += 1;

	return 0;
}

int hio_buf_append_u16_le(struct hio_buf *buf, uint16_t val)
{
	if (hio_buf_get_free(buf) < 2) {
		return -ENOSPC;
	}

	sys_put_le16(val, &buf->mem[buf->len]);
	buf->len += 2;

	return 0;
}

int hio_buf_append_u16_be(struct hio_buf *buf, uint16_t val)
{
	if (hio_buf_get_free(buf) < 2) {
		return -ENOSPC;
	}

	sys_put_be16(val, &buf->mem[buf->len]);
	buf->len += 2;

	return 0;
}

int hio_buf_append_u32_le(struct hio_buf *buf, uint32_t val)
{
	if (hio_buf_get_free(buf) < 4) {
		return -ENOSPC;
	}

	sys_put_le32(val, &buf->mem[buf->len]);
	buf->len += 4;

	return 0;
}

int hio_buf_append_u32_be(struct hio_buf *buf, uint32_t val)
{
	if (hio_buf_get_free(buf) < 4) {
		return -ENOSPC;
	}

	sys_put_be32(val, &buf->mem[buf->len]);
	buf->len += 4;

	return 0;
}

int hio_buf_append_u64_le(struct hio_buf *buf, uint64_t val)
{
	if (hio_buf_get_free(buf) < 8) {
		return -ENOSPC;
	}

	sys_put_le64(val, &buf->mem[buf->len]);
	buf->len += 8;

	return 0;
}

int hio_buf_append_u64_be(struct hio_buf *buf, uint64_t val)
{
	if (hio_buf_get_free(buf) < 8) {
		return -ENOSPC;
	}

	sys_put_be64(val, &buf->mem[buf->len]);
	buf->len += 8;

	return 0;
}

int hio_buf_append_float_le(struct hio_buf *buf, float val)
{
	if (hio_buf_get_free(buf) < 4) {
		return -ENOSPC;
	}

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	sys_mem_swap(&val, sizeof(val));
#endif

	memcpy(&buf->mem[buf->len], &val, 4);
	buf->len += 4;

	return 0;
}

int hio_buf_append_float_be(struct hio_buf *buf, float val)
{
	if (hio_buf_get_free(buf) < 4) {
		return -ENOSPC;
	}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	sys_mem_swap(&val, sizeof(val));
#endif

	memcpy(&buf->mem[buf->len], &val, 4);
	buf->len += 4;

	return 0;
}
