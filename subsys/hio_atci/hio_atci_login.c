/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

#include "hio_atci_login.h"

/* HIO includes */
#include <hio/hio_atci.h>
#include <hio/hio_config.h>

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* Crypto includes */
#if IS_ENABLED(CONFIG_HIO_ATCI_LOGIN_HASH_MBEDTLS)
#include <mbedtls/sha256.h>
#elif IS_ENABLED(CONFIG_HIO_ATCI_LOGIN_HASH_PSA)
#include <psa/crypto.h>
#else
#include <tinycrypt/constants.h>
#include <tinycrypt/sha256.h>
#endif

LOG_MODULE_REGISTER(hio_atci_login, CONFIG_HIO_ATCI_LOG_LEVEL);

#define SETTINGS_PFX "atci"

/* Authorization flag bit requiring an authenticated session. */
#define HIO_ATCI_LOGIN_AUTH_USER BIT(0)

struct hio_atci_login_config g_hio_atci_login_config;
static struct hio_atci_login_config m_config_interim;

static bool m_logged_in;

static struct hio_config_item m_config_items[] = {
	HIO_CONFIG_ITEM_STRING("login-hash", m_config_interim.login_hash,
			       "authentication key (SHA-256 hash hex string)", ""),
};

bool hio_atci_login_is_logged_in(void)
{
	return m_logged_in;
}

void hio_atci_login_logout(void)
{
	m_logged_in = false;
}

/* Compute SHA-256 of @p input and write it as a lowercase hex string (64 chars
 * + terminator, i.e. HIO_ATCI_LOGIN_HASH_SIZE bytes) to @p out. */
static int sha256_hex(const char *input, size_t len, char *out)
{
	uint8_t digest[32];

#if IS_ENABLED(CONFIG_HIO_ATCI_LOGIN_HASH_MBEDTLS)
	int ret;
	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	ret = mbedtls_sha256_starts(&ctx, 0);
	if (ret) {
		LOG_ERR("Call `mbedtls_sha256_starts` failed: %d", ret);
		mbedtls_sha256_free(&ctx);
		return -EINVAL;
	}
	ret = mbedtls_sha256_update(&ctx, (const uint8_t *)input, len);
	if (ret) {
		LOG_ERR("Call `mbedtls_sha256_update` failed: %d", ret);
		mbedtls_sha256_free(&ctx);
		return -EINVAL;
	}
	ret = mbedtls_sha256_finish(&ctx, digest);
	if (ret) {
		LOG_ERR("Call `mbedtls_sha256_finish` failed: %d", ret);
		mbedtls_sha256_free(&ctx);
		return -EINVAL;
	}
	mbedtls_sha256_free(&ctx);
#elif IS_ENABLED(CONFIG_HIO_ATCI_LOGIN_HASH_PSA)
	psa_status_t status;
	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_crypto_init` failed: %d", status);
		return -EINVAL;
	}
	size_t hash_len;
	status = psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)input, len, digest,
				  sizeof(digest), &hash_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_hash_compute` failed: %d", status);
		return -EINVAL;
	}
#else
	int ret;
	struct tc_sha256_state_struct s;
	ret = tc_sha256_init(&s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_init` failed: %d", ret);
		return -EINVAL;
	}
	ret = tc_sha256_update(&s, (const uint8_t *)input, len);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_update` failed: %d", ret);
		return -EINVAL;
	}
	ret = tc_sha256_final(digest, &s);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("Call `tc_sha256_final` failed: %d", ret);
		return -EINVAL;
	}
#endif

	static const char hexdigits[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(digest); i++) {
		out[2 * i] = hexdigits[digest[i] >> 4];
		out[2 * i + 1] = hexdigits[digest[i] & 0x0f];
	}
	out[2 * sizeof(digest)] = '\0';

	return 0;
}

static int hio_atci_login_auth_cb(const struct hio_atci *atci, const struct hio_atci_cmd *cmd,
				  enum hio_atci_cmd_type type, void *user_data)
{
	if (!cmd || cmd->auth_flags == 0) {
		return 0;
	}

	if (m_logged_in) {
		return 0;
	}

	return -EACCES;
}

static int at_login_set(const struct hio_atci *atci, char *argv)
{
	if (!argv) {
		return -EINVAL;
	}

	char *pwd = argv;
	size_t len = strlen(pwd);

	if (len >= 2 && pwd[0] == '"' && pwd[len - 1] == '"') {
		pwd[len - 1] = '\0';
		pwd++;
		len -= 2;
	}

	/* Empty key means no valid password is configured. */
	if (g_hio_atci_login_config.login_hash[0] == '\0') {
		LOG_WRN("ATCI login rejected: no key configured");
		return -EACCES;
	}

	char digest_hex[HIO_ATCI_LOGIN_HASH_SIZE];
	int ret = sha256_hex(pwd, len, digest_hex);
	if (ret) {
		return ret;
	}

	if (strcasecmp(digest_hex, g_hio_atci_login_config.login_hash) != 0) {
		return -EACCES;
	}

	m_logged_in = true;
	LOG_INF("ATCI login OK");
	return 0;
}

HIO_ATCI_CMD_REGISTER(login, "$LOGIN", 0, NULL, at_login_set, NULL, NULL,
		      "Authenticate (AT$LOGIN=\"password\")");

static int at_logout_action(const struct hio_atci *atci)
{
	m_logged_in = false;
	LOG_INF("ATCI logout");
	return 0;
}

HIO_ATCI_CMD_REGISTER(logout, "$LOGOUT", HIO_ATCI_LOGIN_AUTH_USER, at_logout_action, NULL, NULL,
		      NULL, "End authenticated session");

static int init(void)
{
	LOG_INF("System initialization");

	static struct hio_config config = {
		.name = SETTINGS_PFX,
		.items = m_config_items,
		.nitems = ARRAY_SIZE(m_config_items),

		.interim = &m_config_interim,
		.final = &g_hio_atci_login_config,
		.size = sizeof(g_hio_atci_login_config),
	};

	hio_config_register(&config);

	hio_atci_set_auth_check_cb(hio_atci_login_auth_cb, NULL);

	return 0;
}

#ifdef CONFIG_HIO_CONFIG_SHELL

static int print_help(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	shell_help(shell);

	return 0;
}

/* clang-format off */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_atci,

	HIO_CONFIG_SHELL_CMD_ARG,

	SHELL_SUBCMD_SET_END);

/* clang-format on */

SHELL_CMD_REGISTER(atci, &sub_atci, "ATCI commands.", print_help);

/* Load after hio config */
BUILD_ASSERT(CONFIG_HIO_CONFIG_INIT_PRIORITY < CONFIG_HIO_ATCI_LOGIN_INIT_PRIORITY);

#endif /* CONFIG_HIO_CONFIG_SHELL */

SYS_INIT(init, APPLICATION, CONFIG_HIO_ATCI_LOGIN_INIT_PRIORITY);
