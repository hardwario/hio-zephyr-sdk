#include "hio_lte_flow.h"

/* HIO includes */
#include <hio/hio_lte.h>

/* Zephyr includes */
#include <zephyr/sys/util.h>

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
	case HIO_LTE_FSM_EVENT_SOCKET_RECONFIG:
		return "SOCKET_RECONFIG";
	case HIO_LTE_FSM_EVENT_SCAN:
		return "SCAN";
	case HIO_LTE_FSM_EVENT_COPS_DONE:
		return "COPS_DONE";
	case HIO_LTE_FSM_EVENT_DISABLE:
		return "DISABLE";
	case HIO_LTE_FSM_EVENT_COUNT:
		return "for internal use only";
	}
	return INVALID;
}

const char *hio_lte_str_ciphersuite(int ciphersuite)
{
	switch (ciphersuite) {
	case 0xc0a8:
		return "TLS_PSK_WITH_AES_128_CCM_8"; /**< TLS 1.2 */
	default:
		return INVALID;
	}
}

#if defined(CONFIG_HIO_LTE_CAUSE_STR)

struct cause_str {
	uint8_t cause;
	const char *str;
};

static const char *cause_lookup(const struct cause_str *table, size_t len, int cause)
{
	for (size_t i = 0; i < len; i++) {
		if (table[i].cause == cause) {
			return table[i].str;
		}
	}
	return NULL;
}

/* 3GPP TS 24.301 9.9.3.9 */
static const struct cause_str m_emm_causes[] = {
	{2, "IMSI unknown in HSS"},
	{3, "Illegal UE"},
	{5, "IMEI not accepted"},
	{6, "Illegal ME"},
	{7, "EPS services not allowed"},
	{8, "EPS services and non-EPS services not allowed"},
	{9, "UE identity cannot be derived by the network"},
	{10, "Implicitly detached"},
	{11, "PLMN not allowed"},
	{12, "Tracking area not allowed"},
	{13, "Roaming not allowed in this tracking area"},
	{14, "EPS services not allowed in this PLMN"},
	{15, "No suitable cells in tracking area"},
	{16, "MSC temporarily not reachable"},
	{17, "Network failure"},
	{18, "CS domain not available"},
	{19, "ESM failure"},
	{20, "MAC failure"},
	{21, "Synch failure"},
	{22, "Congestion"},
	{23, "UE security capabilities mismatch"},
	{24, "Security mode rejected, unspecified"},
	{25, "Not authorized for this CSG"},
	{26, "Non-EPS authentication unacceptable"},
	{31, "Redirection to 5GCN required"},
	{35, "Requested service option not authorized in this PLMN"},
	{39, "CS service temporarily not available"},
	{40, "No EPS bearer context activated"},
	{42, "Severe network failure"},
	{95, "Semantically incorrect message"},
	{96, "Invalid mandatory information"},
	{97, "Message type non-existent or not implemented"},
	{98, "Message type not compatible with the protocol state"},
	{99, "Information element non-existent or not implemented"},
	{100, "Conditional IE error"},
	{101, "Message not compatible with the protocol state"},
	{111, "Protocol error, unspecified"},
};

static const struct cause_str m_emm_hints[] = {
	{2, "SIM unknown to the network, check SIM activation"},
	{3, "SIM or device rejected, contact the operator"},
	{6, "device rejected (blacklisted?), contact the operator"},
	{7, "SIM has no LTE data service, check the SIM plan"},
	{8, "SIM has no LTE data service, check the SIM plan"},
	{11, "network not allowed for this SIM, check roaming"},
	{12, "area not allowed by subscription, no retry in this PLMN"},
	{13, "roaming not allowed here, modem tries another area"},
	{14, "no LTE roaming in this network, use another operator"},
	{15, "area not allowed, modem searches another area of the operator"},
	{17, "network failure, retry later"},
	{22, "network congestion, retry later"},
	{42, "severe network failure, retry much later"},
};

/* 3GPP TS 24.301 9.9.4.4 */
static const struct cause_str m_esm_causes[] = {
	{8, "Operator determined barring"},
	{26, "Insufficient resources"},
	{27, "Missing or unknown APN"},
	{28, "Unknown PDN type"},
	{29, "User authentication failed"},
	{30, "Request rejected by Serving GW or PDN GW"},
	{31, "Request rejected, unspecified"},
	{32, "Service option not supported"},
	{33, "Requested service option not subscribed"},
	{34, "Service option temporarily out of order"},
	{35, "PTI already in use"},
	{36, "Regular deactivation"},
	{37, "EPS QoS not accepted"},
	{38, "Network failure"},
	{39, "Reactivation requested"},
	{41, "Semantic error in the TFT operation"},
	{42, "Syntactical error in the TFT operation"},
	{43, "Invalid EPS bearer identity"},
	{44, "Semantic errors in packet filter(s)"},
	{45, "Syntactical errors in packet filter(s)"},
	{47, "PTI mismatch"},
	{49, "Last PDN disconnection not allowed"},
	{50, "PDN type IPv4 only allowed"},
	{51, "PDN type IPv6 only allowed"},
	{52, "Single address bearers only allowed"},
	{53, "ESM information not received"},
	{54, "PDN connection does not exist"},
	{55, "Multiple PDN connections for a given APN not allowed"},
	{56, "Collision with network initiated request"},
	{59, "Unsupported QCI value"},
	{60, "Bearer handling not supported"},
	{65, "Maximum number of EPS bearers reached"},
	{66, "Requested APN not supported in current RAT and PLMN combination"},
	{81, "Invalid PTI value"},
	{95, "Semantically incorrect message"},
	{96, "Invalid mandatory information"},
	{97, "Message type non-existent or not implemented"},
	{98, "Message type not compatible with the protocol state"},
	{99, "Information element non-existent or not implemented"},
	{100, "Conditional IE error"},
	{101, "Message not compatible with the protocol state"},
	{111, "Protocol error, unspecified"},
	{112, "APN restriction value incompatible with active EPS bearer context"},
	{113, "Multiple accesses to a PDN connection not allowed"},
};

static const struct cause_str m_esm_hints[] = {
	{8, "barred by the operator, contact the operator"},
	{26, "network out of resources, retry later"},
	{27, "wrong APN, check 'lte config apn'"},
	{28, "wrong PDN type for this APN"},
	{29, "APN login failed, check 'lte config auth/username/password'"},
	{33, "APN not in the SIM plan, check the APN"},
	{66, "APN not available on this RAT/network, check 'lte config mode'"},
};

#define CAUSE_LOOKUP(table, cause) cause_lookup(table, ARRAY_SIZE(table), cause)

#else

#define CAUSE_LOOKUP(table, cause) NULL

#endif /* CONFIG_HIO_LTE_CAUSE_STR */

const char *hio_lte_str_emm_cause(int cause)
{
	const char *str = CAUSE_LOOKUP(m_emm_causes, cause);
	return str ? str : "unknown";
}

const char *hio_lte_str_emm_cause_hint(int cause)
{
	return CAUSE_LOOKUP(m_emm_hints, cause);
}

const char *hio_lte_str_esm_cause(int cause)
{
	const char *str = CAUSE_LOOKUP(m_esm_causes, cause);
	return str ? str : "unknown";
}

const char *hio_lte_str_esm_cause_hint(int cause)
{
	return CAUSE_LOOKUP(m_esm_hints, cause);
}
