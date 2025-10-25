#ifndef SUBSYS_HIO_LTE_PARSE_H_
#define SUBSYS_HIO_LTE_PARSE_H_

/* HIO includes */
#include <hio/hio_lte.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRPS_TIMER_DEACTIVATED -1
#define GRPS_TIMER_INVALID     -2

struct cgdcont_param {
	int cid;          /* CID (context ID) */
	char pdn_type[9]; /* "IP" or "IPV6" */
	char apn[64];     /* Access Point Name */
	char addr[16];    /* IPv4 address */
};

int hio_lte_parse_plmn(const char *str, int *plmn, int16_t *mcc, int16_t *mnc);
int hio_lte_parse_urc_cereg(const char *line, struct hio_lte_cereg_param *param);
int hio_lte_parse_urc_xmodemsleep(const char *line, int *p1, int *p2);
int hio_lte_parse_urc_rai(const char *line, struct hio_lte_rai_param *param);
int hio_lte_parse_coneval(const char *str, struct hio_lte_conn_param *params);
int hio_lte_parse_cgcont(const char *line, struct cgdcont_param *param);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_PARSE_H_ */
