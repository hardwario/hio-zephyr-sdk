#ifndef HIO_INCLUDE_LTE_CONFIG_H_
#define HIO_INCLUDE_LTE_CONFIG_H_

/* Zephyr includes */
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hio_lte_config_auth {
	HIO_LTE_CONFIG_AUTH_NONE = 0,
	HIO_LTE_CONFIG_AUTH_PAP = 1,
	HIO_LTE_CONFIG_AUTH_CHAP = 2,
};

struct hio_lte_config {
	bool test;
	bool nb_iot_mode;
	bool lte_m_mode;
	bool autoconn;
	char plmnid[6 + 1];
	char apn[63 + 1];
	enum hio_lte_config_auth auth;
	char username[32 + 1];
	char password[32 + 1];
	char addr[15 + 1];
	int port;
	bool modemtrace;
};

extern struct hio_lte_config g_hio_lte_config;

int hio_lte_config_init(void);
int hio_lte_config_cmd_show(const struct shell *shell, size_t argc, char **argv);
int hio_lte_config_cmd(const struct shell *shell, size_t argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_LTE_CONFIG_H_ */
