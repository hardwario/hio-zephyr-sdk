#ifndef SUBSYS_HIO_LTE_FLOW_H_
#define SUBSYS_HIO_LTE_FLOW_H_

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

enum hio_lte_fsm_event {
	HIO_LTE_FSM_EVENT_ERROR = 0,
	HIO_LTE_FSM_EVENT_TIMEOUT,
	HIO_LTE_FSM_EVENT_ENABLE,
	HIO_LTE_FSM_EVENT_READY,
	HIO_LTE_FSM_EVENT_SIMDETECTED,
	HIO_LTE_FSM_EVENT_REGISTERED,
	HIO_LTE_FSM_EVENT_DEREGISTERED,
	HIO_LTE_FSM_EVENT_RESET_LOOP,
	HIO_LTE_FSM_EVENT_SOCKET_OPENED,
	HIO_LTE_FSM_EVENT_XMODEMSLEEP,
	HIO_LTE_FSM_EVENT_CSCON_0,
	HIO_LTE_FSM_EVENT_CSCON_1,
	HIO_LTE_FSM_EVENT_XTIME,
	HIO_LTE_FSM_EVENT_SEND,
	HIO_LTE_FSM_EVENT_RECV,
	HIO_LTE_FSM_EVENT_XGPS_ENABLE,
	HIO_LTE_FSM_EVENT_XGPS_DISABLE,
	HIO_LTE_FSM_EVENT_XGPS,
	HIO_LTE_FSM_EVENT_NCELLMEAS,
	HIO_LTE_FSM_EVENT_COUNT /* Must be last */
};

/**
 * @brief Attach/retry timeout configuration.
 */
struct hio_lte_attach_timeout {
	k_timeout_t attach_timeout; /**< Max duration of the current attach attempt. */
	k_timeout_t retry_delay;    /**< Delay before the *next* attempt (after a failure). */
};

typedef void (*HIO_LTE_FSM_EVENT_delegate_cb)(enum hio_lte_fsm_event event);

int hio_lte_flow_init(HIO_LTE_FSM_EVENT_delegate_cb cb);
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
int hio_lte_flow_xmodemtrace(int lvl);

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_periodic(int attempt, k_timeout_t pause);
struct hio_lte_attach_timeout hio_lte_flow_attach_policy_progressive(int attempt);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_HIO_LTE_FLOW_H_ */
