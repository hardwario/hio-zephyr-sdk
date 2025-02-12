#ifndef SUBSYS_HIO_LTE_MODEM_H_
#define SUBSYS_HIO_LTE_MODEM_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hio_lte_conn_param {
	bool valid;
	int result;
	int eest;
	int ecl;
	int rsrp;
	int rsrq;
	int snr;
	int plmn;
	int cid;
	int band;
	int earfcn;
};

enum hio_lte_cereg_param_stat {
	HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED = 0,
	HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME = 1,
	HIO_LTE_CEREG_PARAM_STAT_SEARCHING = 2,
	HIO_LTE_CEREG_PARAM_STAT_REGISTRATION_DENIED = 3,
	HIO_LTE_CEREG_PARAM_STAT_UNKNOWN = 4,
	HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING = 5,
	HIO_LTE_CEREG_PARAM_STAT_SIM_FAILURE = 90,
};

enum hio_lte_cereg_param_act {
	HIO_LTE_CEREG_PARAM_ACT_UNKNOWN = 0,
	HIO_LTE_CEREG_PARAM_ACT_LTE = 7,
	HIO_LTE_CEREG_PARAM_ACT_NBIOT = 9,
};

struct hio_lte_cereg_param {
	bool valid;
	enum hio_lte_cereg_param_stat stat;
	char tac[5];                      // Tracking Area Code (TAC). (hexadecimal format.)
	int cid;                          // Cell Identity (CI) (E-UTRAN cell ID.)
	enum hio_lte_cereg_param_act act; // Access Technology (AcT).
};

struct hio_lte_send_recv_param {
	bool rai;
	const void *send_buf;
	size_t send_len;
	void *recv_buf;
	size_t recv_size;
	size_t *recv_len;
	k_timeout_t timeout;
};

struct hio_lte_metrics {
	uint32_t uplink_count;
	uint32_t uplink_bytes;
	uint32_t uplink_errors;
	int64_t uplink_last_ts;
	uint32_t downlink_count;
	uint32_t downlink_bytes;
	uint32_t downlink_errors;
	int64_t downlink_last_ts;
};

struct hio_lte_attach_timeout {
	k_timeout_t retry_delay;
	k_timeout_t attach_timeout;
};

struct hio_lte_attach_timeout hio_lte_get_curr_attach_timeout(void);

int hio_lte_enable(void);
int hio_lte_wait_for_connected(k_timeout_t timeout);
int hio_lte_send_recv(const struct hio_lte_send_recv_param *param);

// get info
int hio_lte_get_imei(uint64_t *imei);
int hio_lte_get_imsi(uint64_t *imsi);
int hio_lte_get_iccid(char **iccid);
int hio_lte_get_modem_fw_version(char **version);
int hio_lte_get_conn_param(struct hio_lte_conn_param *param);
int hio_lte_get_cereg_param(struct hio_lte_cereg_param *param);
int hio_lte_get_metrics(struct hio_lte_metrics *metrics);

const char *hio_lte_coneval_result_str(int result);

int hio_lte_is_attached(bool *attached);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_MODEM_H_ */
