#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/logging/log.h>

#include <modem/modem_key_mgmt.h>
#include <psa/crypto_types.h>

#include "hio_sec.h"

LOG_MODULE_REGISTER(hio_sec, CONFIG_HIO_SEC_LOG_LEVEL);

int hio_root_cert_write(int security_tag_index, const char *cert, int cert_len)
{
	int ret;

	ret = modem_key_mgmt_write(HIO_SEC_TAG_INDEX, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, cert,
				   cert_len);
	if (ret) {
		LOG_ERR("Call `modem_key_mgmt_write` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_root_cert_delete(int security_tag_index)
{
	int ret;

	ret = modem_key_mgmt_clear(HIO_SEC_TAG_INDEX, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
	if (ret) {
		LOG_ERR("Call `modem_key_mgmt_clear` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_cert_write(int security_tag_index, const char *cert, int cert_len)
{
	int ret;

	ret = modem_key_mgmt_write(HIO_SEC_TAG_INDEX, MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT, cert,
				   cert_len);
	if (ret) {
		LOG_ERR("Call `modem_key_mgmt_write` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_cert_delete(int security_tag_index)
{
	int ret;

	ret = modem_key_mgmt_clear(HIO_SEC_TAG_INDEX, MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT);
	if (ret) {
		LOG_ERR("Call `modem_key_mgmt_clear` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_prv_key_generate(int security_tag_index)
{
	// TODO

	return 0;
}

int hio_prv_key_write(int security_tag_index, const char *key, int key_len)
{
	int ret;

	ret = modem_key_mgmt_write(HIO_SEC_TAG_INDEX, MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_KEY, key,
				   key_len);
	if (ret) {
		LOG_ERR("Call `modem_key_mgmt_write` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_prv_key_delete(int security_tag_index)
{
	int ret;

	ret = modem_key_mgmt_clear(HIO_SEC_TAG_INDEX, MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_KEY);
	if (ret) {
		LOG_ERR("Call `modem_key_mgmt_clear` failed: %d", ret);
		return ret;
	}

	return 0;
}
