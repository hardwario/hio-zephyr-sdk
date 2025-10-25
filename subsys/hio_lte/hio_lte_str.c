#include "hio_lte_flow.h"

/* HIO includes */
#include <hio/hio_lte.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

const char *INVALID = "invalid";

const char *hio_lte_str_coneval_result(int result)
{
	switch (result) {
	case 0:
		return "Connection pre-evaluation successful";
	case 1:
		return "Evaluation failed, no cell available";
	case 2:
		return "Evaluation failed, UICC not available";
	case 3:
		return "Evaluation failed, only barred cells available";
	case 4:
		return "Evaluation failed, busy";
	case 5:
		return "Evaluation failed, aborted because of higher priority operation";
	case 6:
		return "Evaluation failed, not registered";
	case 7:
		return "Evaluation failed, unspecified";
	default:
		return "Evaluation failed, unknown result";
	}
}

const char *hio_lte_str_cereg_stat(enum hio_lte_cereg_param_stat stat)
{
	switch (stat) {
	case HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED:
		return "not-registered";
	case HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME:
		return "registered-home";
	case HIO_LTE_CEREG_PARAM_STAT_SEARCHING:
		return "searching";
	case HIO_LTE_CEREG_PARAM_STAT_REGISTRATION_DENIED:
		return "registration-denied";
	case HIO_LTE_CEREG_PARAM_STAT_UNKNOWN:
		return "unknown";
	case HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING:
		return "registered-roaming";
	case HIO_LTE_CEREG_PARAM_STAT_SIM_FAILURE:
		return "sim-failure";
	default:
		return INVALID;
	}
}

const char *hio_lte_str_cereg_stat_human(enum hio_lte_cereg_param_stat stat)
{
	switch (stat) {
	case HIO_LTE_CEREG_PARAM_STAT_NOT_REGISTERED:
		return "Not registered (idle)";
	case HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME:
		return "Registered (home network)";
	case HIO_LTE_CEREG_PARAM_STAT_SEARCHING:
		return "Searching for network";
	case HIO_LTE_CEREG_PARAM_STAT_REGISTRATION_DENIED:
		return "Registration denied";
	case HIO_LTE_CEREG_PARAM_STAT_UNKNOWN:
		return "Unknown (e.g. out of E-UTRAN coverage)";
	case HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING:
		return "Registered (roaming)";
	case HIO_LTE_CEREG_PARAM_STAT_SIM_FAILURE:
		return "SIM failure";
	default:
		return "Invalid/unsupported status";
	}
}

const char *hio_lte_str_act(enum hio_lte_cereg_param_act act)
{
	switch (act) {
	case HIO_LTE_CEREG_PARAM_ACT_UNKNOWN:
		return "unknown";
	case HIO_LTE_CEREG_PARAM_ACT_LTE:
		return "lte-m";
	case HIO_LTE_CEREG_PARAM_ACT_NBIOT:
		return "nb-iot";
	default:
		return INVALID;
	}
}

const char *hio_lte_str_fsm_event(enum hio_lte_fsm_event event)
{
	switch (event) {
	case HIO_LTE_FSM_EVENT_ERROR:
		return "ERROR";
	case HIO_LTE_FSM_EVENT_TIMEOUT:
		return "TIMEOUT";
	case HIO_LTE_FSM_EVENT_ENABLE:
		return "ENABLE";
	case HIO_LTE_FSM_EVENT_READY:
		return "READY";
	case HIO_LTE_FSM_EVENT_SIMDETECTED:
		return "SIMDETECTED";
	case HIO_LTE_FSM_EVENT_REGISTERED:
		return "REGISTERED";
	case HIO_LTE_FSM_EVENT_DEREGISTERED:
		return "DEREGISTERED";
	case HIO_LTE_FSM_EVENT_RESET_LOOP:
		return "RESET_LOOP";
	case HIO_LTE_FSM_EVENT_SOCKET_OPENED:
		return "SOCKET_OPENED";
	case HIO_LTE_FSM_EVENT_XMODEMSLEEP:
		return "XMODEMSLEEP";
	case HIO_LTE_FSM_EVENT_CSCON_0:
		return "CSCON_0";
	case HIO_LTE_FSM_EVENT_CSCON_1:
		return "CSCON_1";
	case HIO_LTE_FSM_EVENT_XTIME:
		return "XTIME";
	case HIO_LTE_FSM_EVENT_SEND:
		return "SEND";
	case HIO_LTE_FSM_EVENT_RECV:
		return "RECV";
	case HIO_LTE_FSM_EVENT_XGPS_ENABLE:
		return "XGPS_ENABLE";
	case HIO_LTE_FSM_EVENT_XGPS_DISABLE:
		return "XGPS_DISABLE";
	case HIO_LTE_FSM_EVENT_XGPS:
		return "XGPS";
	case HIO_LTE_FSM_EVENT_NCELLMEAS:
		return "NCELLMEAS";
	case HIO_LTE_FSM_EVENT_COUNT:
		return "for internal use only";
	}
	return INVALID;
}
