#ifndef SUBSYS_HIO_LTE_STATE_H_
#define SUBSYS_HIO_LTE_STATE_H_

/* HIO includes */
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int hio_lte_state_get_imei(uint64_t *imei);
void hio_lte_state_set_imei(uint64_t imei);

int hio_lte_state_get_imsi(uint64_t *imsi);
void hio_lte_state_set_imsi(uint64_t imsi);

int hio_lte_state_get_iccid(char **iccid);
void hio_lte_state_set_iccid(const char *iccid);

int hio_lte_state_get_modem_fw_version(char **version);
void hio_lte_state_set_modem_fw_version(const char *version);

int hio_lte_state_get_conn_param(struct hio_lte_conn_param *param);
void hio_lte_state_set_conn_param(const struct hio_lte_conn_param *param);

int hio_lte_state_get_cereg_param(struct hio_lte_cereg_param *param);
void hio_lte_state_set_cereg_param(const struct hio_lte_cereg_param *param);

const char *hio_lte_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_STATE_H_ */
