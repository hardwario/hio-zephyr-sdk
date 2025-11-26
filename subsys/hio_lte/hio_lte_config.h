#ifndef HIO_INCLUDE_LTE_CONFIG_H_
#define HIO_INCLUDE_LTE_CONFIG_H_

/* HIO includes */
#include <hio/hio_lte.h>

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

enum hio_lte_config_powerclass {
	HIO_LTE_CONFIG_POWERCLASS_23_DBM = 0,
	HIO_LTE_CONFIG_POWERCLASS_20_DBM = 1,
};

struct hio_lte_config {
	bool test;
	bool nb_iot_mode;
	bool lte_m_mode;
	char bands[41 + 1];
	char mode[20 + 1];
	char network[6 + 1];
	char apn[63 + 1];
	enum hio_lte_config_auth auth;
	char username[32 + 1];
	char password[32 + 1];
	enum hio_lte_attach_policy attach_policy;
	enum hio_lte_config_powerclass powerclass;

	char addr[15 + 1];
	bool modemtrace;
};

extern struct hio_lte_config g_hio_lte_config;

int hio_lte_config_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_LTE_CONFIG_H_ */
