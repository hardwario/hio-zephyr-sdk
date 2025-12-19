#ifndef HIO_INCLUDE_CLOUD_CONFIG_H_
#define HIO_INCLUDE_CLOUD_CONFIG_H_

#include "hio_cloud_transfer.h"

/* HIO includes */
#include <hio/hio_cloud.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hio_cloud_protocol {
	HIO_CLOUD_PROTOCOL_FLAP_HASH = 0,
	HIO_CLOUD_PROTOCOL_FLAP_DTLS,
};

struct hio_cloud_config {
	enum hio_cloud_protocol protocol;
	char addr[40];
	int port_signed;
	int port_dtls;
};

extern struct hio_cloud_config g_hio_cloud_config;

int hio_cloud_config_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HIO_INCLUDE_CLOUD_CONFIG_H_ */
