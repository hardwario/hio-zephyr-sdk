/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* HIO includes */
#include <hio/hio_tok.h>

/* Standard includes */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *hio_tok_pfx(const char *s, const char *pfx)
{
	if (!s || !pfx) {
		return NULL;
	}

	size_t len = strlen(pfx);

	if (len == 0 || strncmp(s, pfx, len) != 0) {
		return NULL;
	}

	return s + len;
}

const char *hio_tok_sep(const char *s)
{
	if (!s || *s != ',') {
		return NULL;
	}
	return s + 1;
}

const char *hio_tok_end(const char *s)
{
	if (!s || *s != '\0') {
		return NULL;
	}
	return s;
}

const char *hio_tok_str(const char *s, bool *def, char *str, size_t size)
{
	if (!s) {
		return NULL;
	}

	if (def) {
		*def = false;
	}

	if (str && size > 0) {
		str[0] = '\0';
	}

	if (*s == '\0' || *s == ',') {
		return s;
	}

	if (*s != '"') {
		return NULL;
	}

	const char *end_quote = strchr(s + 1, '"');
	if (!end_quote) {
		return NULL;
	}

	if (end_quote[1] != '\0' && end_quote[1] != ',') {
		return NULL;
	}

	if (def) {
		*def = true;
	}

	if (str) {
		size_t len = (size_t)(end_quote - (s + 1));
		if (len >= size) {
			return NULL;
		}
		strncpy(str, s + 1, len);
		str[len] = '\0';
	}

	return end_quote + 1;
}

const char *hio_tok_num(const char *s, bool *def, long *num)
{
	if (!s) {
		return NULL;
	}

	if (def) {
		*def = false;
	}
	if (num) {
		*num = 0;
	}

	if (*s == '\0' || *s == ',') {
		return s;
	}

	if (!isdigit((int)*s) && *s != '-') {
		return NULL;
	}

	char *end;
	long value = strtol(s, &end, 10);

	if (*end != '\0' && *end != ',') {
		return NULL;
	}

	if (def) {
		*def = true;
	}
	if (num) {
		*num = value;
	}

	return end;
}

const char *hio_tok_uint(const char *s, bool *def, uint32_t *num)
{
	if (!s) {
		return NULL;
	}

	if (def) {
		*def = false;
	}
	if (num) {
		*num = 0;
	}

	if (*s == '\0' || *s == ',') {
		return s;
	}

	char *end;
	int base = 10;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}

	if (!isxdigit((int)*s)) {
		return NULL;
	}

	uint32_t value = (uint32_t)strtoul(s, &end, base);

	if (*end != '\0' && *end != ',') {
		return NULL;
	}

	if (def) {
		*def = true;
	}
	if (num) {
		*num = value;
	}

	return end;
}

const char *hio_tok_float(const char *s, bool *def, float *num)
{
	if (!s) {
		return NULL;
	}

	if (def) {
		*def = false;
	}
	if (num) {
		*num = 0;
	}

	if (*s == '\0' || *s == ',') {
		return s;
	}

	char *end;
	float value = strtof(s, &end);

	if (end == s || (*end != '\0' && *end != ',')) {
		return NULL;
	}

	if (def) {
		*def = true;
	}
	if (num) {
		*num = value;
	}

	return end;
}

static int hex_char_to_value(char c, uint8_t *val)
{
	if (c >= '0' && c <= '9') {
		*val = c - '0';
		return 0;
	}
	if (c >= 'a' && c <= 'f') {
		*val = c - 'a' + 10;
		return 0;
	}
	if (c >= 'A' && c <= 'F') {
		*val = c - 'A' + 10;
		return 0;
	}
	return -1;
}

const char *hio_tok_hex(const char *s, bool *def, void *buffer, size_t buf_len)
{
	if (!s || !buffer) {
		return NULL;
	}

	if (def) {
		*def = false;
	}
	uint8_t *buf = (uint8_t *)buffer;
	size_t i = 0;

	while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1])) {
		if (i >= buf_len) {
			return NULL;
		}

		uint8_t hi, lo;
		if (hex_char_to_value(s[0], &hi) != 0 || hex_char_to_value(s[1], &lo) != 0) {
			return NULL;
		}

		buf[i++] = (hi << 4) | lo;
		s += 2;
	}

	if (def) {
		*def = (i > 0);
	}
	return s;
}

bool hio_tok_is_quoted(const char *s)
{
	return s && *s == '"';
}

bool hio_tok_is_empty(const char *s)
{
	return !s || *s == '\0' || *s == ',';
}
