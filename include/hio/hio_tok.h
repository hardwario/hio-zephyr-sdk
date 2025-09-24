#ifndef LIB_HIO_TOK_H_
#define LIB_HIO_TOK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup hio_tok hio_tok
 * @{
 */

/**
 * @brief Checks if a string starts with a given prefix.
 *
 * This function returns a pointer to the string if the prefix matches, or NULL otherwise.
 *
 * @param s     The input string to check.
 * @param pfx   The prefix to check for.
 * @return      Pointer to the character after the prefix if it matches, or NULL if not.
 */
const char *hio_tok_pfx(const char *s, const char *pfx);

/**
 * @brief @brief Checks if the character in the string is a comma
 *
 * If a separator is found, advances to the next character.
 *
 * @param s     The input string.
 * @return      Pointer to the next character if separator found, or NULL if not.
 */
const char *hio_tok_sep(const char *s);

/**
 * @brief Checks if the parser has reached the end of the string.
 *
 * @param s     The input string.
 * @return      Pointer to input if end (`*s == '\0'`), otherwise NULL.
 */
const char *hio_tok_end(const char *s);

/**
 * @brief Extracts a string enclosed in double quotes and stores it in a buffer.
 *
 * The quotes are removed. If the field is empty or missing, 'def' will be set to false.
 *
 * @param s     The input string.
 * @param def   Output: true if a string was found and copied.
 * @param str   Destination buffer for the string.
 * @param size  Size of the buffer.
 * @return      Pointer to the next character, or NULL on error.
 */
const char *hio_tok_str(const char *s, bool *def, char *str, size_t size);

/**
 * @brief Parses a signed number from the string.
 *
 * Accepts decimal format only. Sets `def` to true if a number is found.
 *
 * @param s     The input string.
 * @param def   Output: true if a number was present.
 * @param num   Output value.
 * @return      Pointer to the next character, or NULL on error.
 */
const char *hio_tok_num(const char *s, bool *def, long *num);

/**
 * @brief Parses a floating-point number from the string.
 *
 * Sets `def` to true if a number was found. Supports `,` or `\0` as end token.
 *
 * @param s     The input string.
 * @param def   Output: true if a number was present.
 * @param num   Output value.
 * @return      Pointer to the next character, or NULL on error.
 */
const char *hio_tok_float(const char *s, bool *def, float *num);

/**
 * @brief Parses an unsigned integer (decimal or 0x-prefixed hex).
 *
 * Sets `def` to true if a value was found. Accepts '0x' prefix for hexadecimal.
 *
 * @param s     The input string.
 * @param def   Output: true if value was present.
 * @param num   Output value.
 * @return      Pointer to next character, or NULL on error.
 */
const char *hio_tok_uint(const char *s, bool *def, uint32_t *num);

/**
 * @brief Parses a hexadecimal string into a binary buffer.
 *
 * The input must contain even number of valid hex digits inside quotes.
 *
 * @param s         Input string with hex characters (e.g., "AABBCC").
 * @param def       Output: true if value was present.
 * @param buffer    Output buffer for binary data.
 * @param buf_len   Size of the output buffer.
 * @param out_len   Output: number of bytes written to the buffer.
 * @return          Pointer to next character, or NULL on error.
 */
const char *hio_tok_hex(const char *s, bool *def, void *buffer, size_t buf_len, size_t *out_len);

/**
 * @brief Parses a Base64-encoded string into a binary buffer.
 *
 * The input must be valid Base64 inside quotes.
 *
 * @param s         Input string with Base64 characters.
 * @param buffer    Output buffer for binary data.
 * @param buf_len   Size of the output buffer.
 * @param out_len   Output: number of bytes written to the buffer.
 * @return          Pointer to next character, or NULL on error.
 */
const char *hio_tok_base64(const char *s, void *buffer, size_t buf_len, size_t *out_len);

/**
 * @brief Checks if the input begins with a quotation mark.
 *
 * @param s     Input string.
 * @return      true if the first character is '"', false otherwise.
 */
bool hio_tok_is_quoted(const char *s);

/**
 * @brief Checks if the current field is empty or at a separator.
 *
 * Useful for detecting missing/optional arguments.
 *
 * @param s     Input string.
 * @return      true if the current character is '\0' or ',', false otherwise.
 */
bool hio_tok_is_empty(const char *s);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* LIB_HIO_TOK_H_ */
