#ifndef SUBSYS_HIO_LTE_FLOW_H_
#define SUBSYS_HIO_LTE_FLOW_H_

/* HIO includes */
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	HIO_LTE_ERR_NONE = 0,

	HIO_LTE_ERR_MODEM_INACTIVE = 1000,       /* CFUN != 1 */
	HIO_LTE_ERR_CEREG_NOT_SUBSCRIBED = 1001, /* CEREG unsolicited codes off */
	HIO_LTE_ERR_CEREG_NOT_REGISTERED = 1002, /* not registered to network */

	HIO_LTE_ERR_PDN_NOT_ATTACHED = 1003, /* CGATT != 1 */
	HIO_LTE_ERR_PDN_NOT_ACTIVE = 1004,   /* CGACT != 0,1 */

	HIO_LTE_ERR_SOCKET_ERROR = 1005, /* socket check failed */
};

enum hio_lte_event {
	HIO_LTE_EVENT_ERROR = 0,
	HIO_LTE_EVENT_TIMEOUT,
	HIO_LTE_EVENT_ENABLE,
	HIO_LTE_EVENT_READY,
	HIO_LTE_EVENT_SIMDETECTED,
	HIO_LTE_EVENT_REGISTERED,
	HIO_LTE_EVENT_DEREGISTERED,
	HIO_LTE_EVENT_RESET_LOOP,
	HIO_LTE_EVENT_SOCKET_OPENED,
	HIO_LTE_EVENT_XMODEMSLEEP,
	HIO_LTE_EVENT_CSCON_0,
	HIO_LTE_EVENT_CSCON_1,
	HIO_LTE_EVENT_XTIME,
	HIO_LTE_EVENT_SEND,
	HIO_LTE_EVENT_RECV,
	HIO_LTE_EVENT_XGPS_ENABLE,
	HIO_LTE_EVENT_XGPS_DISABLE,
	HIO_LTE_EVENT_XGPS,
};

#define GRPS_TIMER_DEACTIVATED -1
#define GRPS_TIMER_INVALID     -2

typedef void (*hio_lte_flow_event_delegate_cb)(enum hio_lte_event event);
int hio_lte_flow_init(hio_lte_flow_event_delegate_cb cb);
int hio_lte_flow_start(void);
int hio_lte_flow_stop(void);

int hio_lte_flow_prepare(void);
int hio_lte_flow_cfun(int cfun);
int hio_lte_flow_sim_info(void);
int hio_lte_flow_sim_fplmn(void);
int hio_lte_flow_open_socket(void);

int hio_lte_flow_check(void);
int hio_lte_flow_send(const struct hio_lte_send_recv_param *param);
int hio_lte_flow_recv(const struct hio_lte_send_recv_param *param);

int hio_lte_flow_coneval(void);
int hio_lte_flow_cmd(const char *cmd);

int hio_lte_flow_cmd_test_cmd(const struct shell *shell, size_t argc, char **argv);
int hio_lte_flow_cmd_trace(const struct shell *shell, size_t argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_FLOW_H_ */
