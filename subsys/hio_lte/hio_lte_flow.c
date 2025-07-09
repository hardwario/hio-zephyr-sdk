#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_state.h"
#include "hio_lte_talk.h"
#include "hio_lte_tok.h"

/* HIO includes */
#include <hio/hio_rtc.h>
#include <hio/hio_lte.h>
#include <hio/hio_tok.h>

/* nRF includes */
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <modem/at_monitor.h>
#include <modem/at_parser.h>
#include <nrf_socket.h>
#include <nrf_errno.h>

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/net/socket_ncs.h>
#include <zephyr/sys/timeutil.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(hio_lte_flow, CONFIG_HIO_LTE_LOG_LEVEL);

AT_MONITOR(hio_lte_flow, ANY, monitor_handler);

struct cgdcont_param {
	int cid;          /* CID (context ID) */
	char pdn_type[9]; /* "IP" or "IPV6" */
	char apn[64];     /* Access Point Name */
	char addr[16];    /* IPv4 address */
};

#define XSLEEP_PAUSE K_MSEC(100)

#define XRECVFROM_TIMEOUT_SEC 5

#define SOCKET_SEND_TMO_SEC  30
#define RESPONSE_TIMEOUT_SEC 5

static K_EVENT_DEFINE(m_flow_events);

static hio_lte_flow_event_delegate_cb m_event_delegate_cb;

static K_MUTEX_DEFINE(m_addr_info_lock);
static struct nrf_sockaddr_in m_addr_info;
static struct cgdcont_param m_cgdcont;

static int m_socket_fd;

int cellid_hex2int(char *ci, size_t size, int *cid)
{
	if (!ci || size != 9 || !cid) {
		return -EINVAL;
	}

	uint8_t cell_id_buf[4];

	size_t n = hex2bin(ci, 8, cell_id_buf, sizeof(cell_id_buf));

	if (n != sizeof(cell_id_buf)) {
		return -EINVAL;
	}

	*cid = sys_get_be32(cell_id_buf);

	return 0;
}

int parse_gprs_timer(const char *binary_string, int flag)
{
	if (strlen(binary_string) != 8) {
		return GRPS_TIMER_INVALID;
	}

	int byte_value = (int)strtol(binary_string, NULL, 2);
	int time_unit = (byte_value >> 5) & 0x07;
	int timer_value = byte_value & 0x1F;

	if (time_unit == 0b111) {
		return GRPS_TIMER_DEACTIVATED;
	}

	int multiplier = 0;
	if (flag == 2) {
		/* GPRS Timer 2 (Active-Time) */
		switch (time_unit) {
		case 0b000:
			multiplier = 2;
			break; /* 2 seconds */
		case 0b001:
			multiplier = 60;
			break; /* 1 minute */
		case 0b010:
			multiplier = 360;
			break; /* 6 minutes */
		default:
			return GRPS_TIMER_INVALID;
		}
	} else if (flag == 3) {
		/* GPRS Timer 3 (Periodic-TAU-ext) */
		switch (time_unit) {
		case 0b000:
			multiplier = 600;
			break; /* 10 minutes */
		case 0b001:
			multiplier = 3600;
			break; /* 1 hour */
		case 0b010:
			multiplier = 36000;
			break; /* 10 hours */
		case 0b011:
			multiplier = 2;
			break; /* 2 seconds */
		case 0b100:
			multiplier = 30;
			break; /* 30 seconds */
		case 0b101:
			multiplier = 60;
			break; /* 1 minute */
		case 0b110:
			multiplier = 1152000;
			break; /* 320 hours */
		default:
			return GRPS_TIMER_INVALID;
		}
	} else {
		return GRPS_TIMER_INVALID;
	}

	return timer_value * multiplier;
}

static int parse_cereg(const char *line, struct hio_lte_cereg_param *param)
{
	/* <stat>[,[<tac>],[<ci>],[<AcT>][,<cause_type>],[<reject_cause>][,[<Active-Time>],[<Periodic-TAU-ext>]]]]
	5,"AF66","009DE067",9,,,"00000000","00111000"
	2,"B4DC","000AE520",9 */

	if (!line || !param) {
		return -EINVAL;
	}

	memset(param, 0, sizeof(*param));

	const char *p = line;

	bool def;
	long num;
	char str[8 + 1];

	if (!(p = hio_tok_num(p, &def, &num)) || !def) {
		LOG_ERR("Failed to parse stat");
		return -EINVAL;
	}

	param->stat = (enum hio_lte_cereg_param_stat)num;

	if (!(p = hio_tok_sep(p))) {
		param->valid = true;
		return 0;
	}

	if (!(p = hio_tok_str(p, &def, param->tac, sizeof(param->tac))) || !def) {
		return -EINVAL;
	}

	if (!(p = hio_tok_sep(p))) {
		return -EINVAL;
	}

	if (!(p = hio_tok_str(p, &def, str, sizeof(str))) || !def) {
		return -EINVAL;
	}

	if (cellid_hex2int(str, sizeof(str), &param->cid) != 0) {
		return -EINVAL;
	}

	if (!(p = hio_tok_sep(p))) {
		return -EINVAL;
	}

	if (!(p = hio_tok_num(p, &def, &num)) || !def) {
		return -EINVAL;
	}

	param->act = (enum hio_lte_cereg_param_act)num;

	if (!(p = hio_tok_sep(p))) {
		param->valid = true;
		return 0;
	}

	if (!(p = hio_tok_num(p, &def, &num)) && !def) {
		return -EINVAL;
	}

	param->cause_type = num;

	if (!(p = hio_tok_sep(p))) {
		return 0;
	}

	if (!(p = hio_tok_num(p, &def, &num)) && !def) {
		return -EINVAL;
	}

	param->reject_cause = num;

	if (!(p = hio_tok_sep(p))) {
		param->valid = true;
		return 0;
	}

	if (!(p = hio_tok_str(p, &def, str, sizeof(str))) || !def) {
		return -EINVAL;
	}

	param->active_time = parse_gprs_timer(str, 2);

	if (!(p = hio_tok_sep(p))) {
		return -EINVAL;
	}

	if (!(p = hio_tok_str(p, &def, str, sizeof(str))) || !def) {
		return -EINVAL;
	}

	param->periodic_tau_ext = parse_gprs_timer(str, 3);

	if (!hio_tok_end(p)) {
		return -EINVAL;
	}

	param->valid = true;

	return 0;
}

static int parse_xmodemsleep(const char *line, int *p1, int *p2)
{
	int ret;

	if (!line) {
		return -EINVAL;
	}

	int p1_ = 0;
	int p2_ = 0;

	ret = sscanf(line, "%d,%d", &p1_, &p2_);
	if (ret != 1 && ret != 2) {
		LOG_ERR("Failed to parse xmodemsleep: %d", ret);
		return -EINVAL;
	}

	if (p1) {
		*p1 = p1_;
	}

	if (p2) {
		*p2 = p2_;
	}

	return 0;
}

static int parse_rai(const char *line, struct hio_lte_rai_param *param)
{
	int ret;
	char cell_id[9];
	char plmn[6];
	int as_rai;
	int cp_rai;

	if (!line || !param) {
		return -EINVAL;
	}

	memset(param, 0, sizeof(*param));
	ret = sscanf(line, "\"%8[0-9A-F]\",\"%5[0-9A-F]\",%d,%d", cell_id, plmn, &as_rai, &cp_rai);
	if (ret != 4) {
		LOG_ERR("Failed to parse rai: %d", ret);
		return -EINVAL;
	}

	char *tol_end = NULL;
	int cell_id_int = strtol(cell_id, &tol_end, 16);
	if (tol_end != cell_id + 8) {
		LOG_ERR("Failed to parse cell_id parameter");
		return -EINVAL;
	}

	param->cell_id = cell_id_int;

	tol_end = NULL;
	long plmn_int = strtol(plmn, &tol_end, 10);
	if (tol_end != plmn + 5) {
		LOG_ERR("Failed to parse plmn parameter");
		return -EINVAL;
	}

	param->plmn = (int)plmn_int;

	param->as_rai = as_rai != 0;
	param->cp_rai = cp_rai != 0;

	param->valid = true;

	return 0;
}

static int parse_cgcont(const char *line, struct cgdcont_param *param)
{
	/* 0,"IP","iot.1nce.net","10.52.2.149",0,0 */
	if (!line || !param) {
		return -EINVAL;
	}

	memset(param, 0, sizeof(*param));
	param->cid = -1; /* Default CID is -1 (not set) */

	const char *p = line;

	bool def;
	long num;

	if (!(p = hio_tok_num(p, &def, &num)) || !def) {
		LOG_ERR("Failed to parse CID");
		return -EINVAL;
	}

	param->cid = (int)num;

	if (!(p = hio_tok_sep(p))) {
		LOG_ERR("Failed to parse PDN type");
		return -EINVAL;
	}

	if (!(p = hio_tok_str(p, &def, param->pdn_type, sizeof(param->pdn_type))) || !def) {
		LOG_ERR("Failed to parse PDN type");
		return -EINVAL;
	}

	if (!(p = hio_tok_sep(p))) {
		LOG_ERR("Failed to parse APN");
		return -EINVAL;
	}

	if (!(p = hio_tok_str(p, &def, param->apn, sizeof(param->apn))) || !def) {
		LOG_ERR("Failed to parse APN");
		return -EINVAL;
	}

	if (!(p = hio_tok_sep(p))) {
		LOG_ERR("Failed to parse address");
		return -EINVAL;
	}

	if (!(p = hio_tok_str(p, &def, param->addr, sizeof(param->addr))) || !def) {
		LOG_ERR("Failed to parse address");
		return -EINVAL;
	}

	return 0;
}

static void monitor_handler(const char *line)
{
	int ret;

	for (size_t i = 0; i < strlen(line); i++) {
		if (line[i] == '\r' || line[i] == '\n') {
			((char *)line)[i] = '\0';
		}
	}

	LOG_INF("URC: %s", line);

	if (!strcmp(line, "Ready")) {
		m_event_delegate_cb(HIO_LTE_EVENT_READY);
	} else if (!strncmp(line, "%XSIM: 1", 8)) {
		m_event_delegate_cb(HIO_LTE_EVENT_SIMDETECTED);
	} else if (!strncmp(line, "%XTIME:", 7)) {
		m_event_delegate_cb(HIO_LTE_EVENT_XTIME);
	} else if (!strncmp(line, "+CEREG: ", 8)) {
		struct hio_lte_cereg_param cereg_param = {0};

		ret = parse_cereg(&line[8], &cereg_param);
		if (ret) {
			LOG_WRN("Call `parse_cereg` failed: %d", ret);
			return;
		}

		if (!cereg_param.valid) {
			LOG_WRN("CEREG was %d\n", (enum hio_lte_cereg_param_stat)cereg_param.stat);
			return;
		}

		hio_lte_state_set_cereg_param(&cereg_param);

		if (cereg_param.stat == HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME ||
		    cereg_param.stat == HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING) {
			m_event_delegate_cb(HIO_LTE_EVENT_REGISTERED);
		} else {
			m_event_delegate_cb(HIO_LTE_EVENT_DEREGISTERED);
		}
	} else if (!strncmp(line, "%MDMEV: ", 8)) {
		if (!strncmp(&line[8], "RESET LOOP", 10)) {
			LOG_WRN("Modem reset loop detected");
			m_event_delegate_cb(HIO_LTE_EVENT_RESET_LOOP);
		}
	} else if (!strncmp(line, "+CSCON: 0", 9)) {
		m_event_delegate_cb(HIO_LTE_EVENT_CSCON_0);
	} else if (!strncmp(line, "+CSCON: 1", 9)) {
		m_event_delegate_cb(HIO_LTE_EVENT_CSCON_1);
	} else if (!strncmp(line, "%XMODEMSLEEP: ", 14)) {
		int p1 = 0, p2 = 0;

		ret = parse_xmodemsleep(line + 14, &p1, &p2);
		if (ret) {
			LOG_WRN("Call `parse_xmodemsleep` failed: %d", ret);
			return;
		}
		if (p2 > 0 || p1 == 4) {
			m_event_delegate_cb(HIO_LTE_EVENT_XMODEMSLEEP);
		}
	} else if (!strncmp(line, "%RAI: ", 6)) {
		struct hio_lte_rai_param rai_param = {0};
		ret = parse_rai(line + 6, &rai_param);
		if (ret) {
			LOG_WRN("Call `parse_rai` failed: %d", ret);
			return;
		}

		hio_lte_state_set_rai_param(&rai_param);
	}
}

static void str_remove_trailing_quotes(char *str)
{
	int l = strlen(str);
	if (l > 0 && str[0] == '"' && str[l - 1] == '"') {
		str[l - 1] = '\0';        /* Remove trailing quote */
		memmove(str, str + 1, l); /* Remove leading quote */
	}
}

int hio_lte_flow_start(void)
{
	int ret;

	if (nrf_modem_is_initialized()) {
		return 0;
	}

	ret = nrf_modem_lib_init();
	if (ret) {
		LOG_ERR("Call `nrf_modem_lib_init` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_flow_stop(void)
{
	int ret;

	if (!nrf_modem_is_initialized()) {
		return 0;
	}

	ret = hio_lte_talk_at_cfun(0);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cfun: 0` failed: %d", ret);
		return ret;
	}

	ret = nrf_modem_lib_shutdown();
	if (ret) {
		LOG_ERR("Call `nrf_modem_lib_shutdown` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int fill_bands(char *bands)
{
	size_t len = strlen(bands);
	const char *p = g_hio_lte_config.bands;
	bool def;
	long band;
	while (p) {
		if (!(p = hio_lte_tok_num(p, &def, &band)) || !def || band < 0 || band > 255) {
			LOG_ERR("Invalid number format");
			return -EINVAL;
		}

		LOG_INF("Band: %ld", band);

		int n = len - band; /* band 1 is first 1 in bands from right */
		if (n < 0) {
			LOG_ERR("Invalid band number");
			return -EINVAL;
		}

		bands[n] = '1';

		if (hio_lte_tok_end(p)) {
			break;
		}

		if (!(p = hio_lte_tok_sep(p))) {
			LOG_ERR("Expected comma");
			return -EINVAL;
		}
	}

	return 0;
}

int hio_lte_flow_prepare(void)
{
	int ret;

	ret = hio_lte_talk_at_cfun(0);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cfun: 0` failed: %d", ret);
		return ret;
	}

	char cgsn[64] = {0};
	ret = hio_lte_talk_at_cgsn(cgsn, sizeof(cgsn));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cgsn` failed: %d", ret);
		return ret;
	}

	str_remove_trailing_quotes(cgsn);

	LOG_INF("CGSN: %s", cgsn);

	hio_lte_state_set_imei(strtoull(cgsn, NULL, 10));

	char hw_version[64] = {0};
	ret = hio_lte_talk_at_hwversion(hw_version, sizeof(hw_version));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_hwversion` failed: %d", ret);
		return ret;
	}

	LOG_INF("HW version: %s", hw_version);

	char sw_version[64] = {0};
	ret = hio_lte_talk_at_shortswver(sw_version, sizeof(sw_version));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_shortswver` failed: %d", ret);
		return ret;
	}

	LOG_INF("SW version: %s", sw_version);

	ret = hio_lte_talk_at_xpofwarn(1, 30);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xpofwarn` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_xtemphighlvl(70);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xtemphighlvl` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_xtemp(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xtemp` failed: %d", ret);
		return ret;
	}

	int gnss_mode = 0;

	char *pos_let_m = strstr(g_hio_lte_config.mode, "lte-m");
	char *pos_nb_iot = strstr(g_hio_lte_config.mode, "nb-iot");

	int lte_m_mode = pos_let_m ? 1 : 0;
	int nb_iot_mode = pos_nb_iot ? 1 : 0;
	int preference = 0;
	if (pos_let_m && pos_nb_iot) {
		preference = pos_let_m < pos_nb_iot ? 1 : 2;
	}

	ret = hio_lte_talk_at_xsystemmode(lte_m_mode, nb_iot_mode, gnss_mode, preference);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xsystemmode` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_xdataprfl(0);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xdataprfl` failed: %d", ret);
		return ret;
	}

	if (!strlen(g_hio_lte_config.bands)) {
		ret = hio_lte_talk_at_xbandlock(0, NULL);
	} else {
		char bands[] =
			"00000000000000000000000000000000000000000000000000000000000000000000"
			"00000000000000000000";

		ret = fill_bands(bands);
		if (ret) {
			LOG_ERR("Call `fill_bands` failed: %d", ret);
			return ret;
		}
		ret = hio_lte_talk_at_xbandlock(1, bands);
	}
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xbandlock` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_xsim(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xsim` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_xnettime(1, NULL);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xnettime` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_mdmev(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_mdmev` failed: %d", ret);
		return ret;
	}

	/* Enable RAI with notifications */
	ret = hio_lte_talk_at_rai(2);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_rai` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cpsms((int[]){1}, "00111000", "00000000");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cpsms` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_ceppi(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_ceppi` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cereg(5);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cereg` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cgerep(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cgerep` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cmee(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmee` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cnec(24);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cnec` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cscon(1);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cscon` failed: %d", ret);
		return ret;
	}

	if (!strlen(g_hio_lte_config.network)) {
		ret = hio_lte_talk_at_cops(0, NULL, NULL);
	} else {
		ret = hio_lte_talk_at_cops(1, (int[]){2}, g_hio_lte_config.network);
	}
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cops` failed: %d", ret);
		return ret;
	}

	/* subscribes modem sleep notifications */
	ret = hio_lte_talk_at_xmodemsleep(1, (int[]){500}, (int[]){10240});
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xmodemsleep` failed: %d", ret);
		return ret;
	}

	if (!strlen(g_hio_lte_config.apn)) {
		ret = hio_lte_talk_at_cgdcont(0, "IP", NULL);
	} else {
		ret = hio_lte_talk_at_cgdcont(0, "IP", g_hio_lte_config.apn);
	}
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cgdcont` failed: %d", ret);
		return ret;
	}

	if (g_hio_lte_config.auth == HIO_LTE_CONFIG_AUTH_PAP ||
	    g_hio_lte_config.auth == HIO_LTE_CONFIG_AUTH_CHAP) {
		int protocol = g_hio_lte_config.auth == HIO_LTE_CONFIG_AUTH_PAP ? 1 : 2;
		ret = hio_lte_talk_at_cgauth(0, &protocol, g_hio_lte_config.username,
					     g_hio_lte_config.password);
		if (ret) {
			LOG_ERR("Call `hio_lte_talk_at_cgauth` failed: %d", ret);
			return ret;
		}
	} else {
		ret = hio_lte_talk_at_cgauth(0, (int[]){0}, NULL, NULL);
		if (ret) {
			LOG_ERR("Call `hio_lte_talk_at_cgauth` failed: %d", ret);
			return ret;
		}
	}

	return 0;
}

int hio_lte_flow_cfun(int cfun)
{
	int ret;

	ret = hio_lte_talk_at_cfun(cfun);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cfun` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_flow_sim_info(void)
{
	int ret;
	char cimi[64] = {0};
	uint64_t imsi, imsi_test = 0;

	for (int i = 0; i < 10; i++) {
		memset(cimi, 0, sizeof(cimi));
		ret = hio_lte_talk_at_cimi(cimi, sizeof(cimi));
		if (ret) {
			LOG_ERR("Call `hio_lte_talk_at_cimi` failed: %d", ret);
			return ret;
		}

		imsi = strtoull(cimi, NULL, 10);

		if (imsi != 0 && imsi == imsi_test) {
			break;
		}

		imsi_test = imsi;
	}

	LOG_INF("CIMI: %llu", imsi);

	hio_lte_state_set_imsi(imsi);

	char iccid[64] = {0};
	ret = hio_lte_talk_at_iccid(iccid, sizeof(iccid));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_iccid` failed: %d", ret);
		return ret;
	}

	if (strlen(iccid) < 18 || strlen(iccid) > 22) {
		LOG_ERR("Invalid ICCID: %s", iccid);
		return -EINVAL;
	}

	LOG_INF("ICCID: %s", iccid);

	hio_lte_state_set_iccid(iccid);

	return 0;
}

int hio_lte_flow_sim_fplmn(void)
{
	/* Test FPLMN (forbidden network) list on a SIM */
	int ret;
	char crsm_144[32];

	ret = hio_lte_talk_crsm_176(crsm_144, sizeof(crsm_144));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_crsm_176` failed: %d", ret);
		return ret;
	}
	if (strcmp(crsm_144, "\"FFFFFFFFFFFFFFFFFFFFFFFF\"")) {
		LOG_WRN("Found forbidden network(s) - erasing");

		/* FPLMN erase */
		ret = hio_lte_talk_crsm_214();
		if (ret) {
			LOG_ERR("Call `hio_lte_talk_crsm_214` failed: %d", ret);
			return -EOPNOTSUPP;
		}

		ret = hio_lte_talk_at_cfun(4);
		if (ret) {
			LOG_ERR("Call `hio_lte_talk_at_cfun: 4` failed: %d", ret);
			return ret;
		}

		k_sleep(K_MSEC(100));

		ret = hio_lte_talk_at_cfun(1);
		if (ret) {
			LOG_ERR("Call `hio_lte_talk_at_cfun: 1` failed: %d", ret);
			return ret;
		}

		return -EAGAIN;
	}

	return 0;
}

static int update_cgdcont(void)
{
	int ret;
	char tmp[200] = {0};
	int lines = hio_lte_talk_at_cgdcont_q(tmp, sizeof(tmp));
	char *line = tmp;
	for (int i = 0; i < lines; i++) {
		ret = parse_cgcont(line, &m_cgdcont);
		if (ret) {
			LOG_ERR("Call `parse_cgcont` failed: %d", ret);
			return ret;
		}

		LOG_INF("CID: %d, PDN type: %s, APN: %s, Address: %s", m_cgdcont.cid,
			m_cgdcont.pdn_type, m_cgdcont.apn, m_cgdcont.addr);

		if (m_cgdcont.cid != -1 && strlen(m_cgdcont.pdn_type) == 2 &&
		    strcmp(m_cgdcont.pdn_type, "IP") == 0 && strlen(m_cgdcont.apn) > 0 &&
		    strlen(m_cgdcont.addr) > 0) {
			return 0;
		}

		line = &tmp[strlen(line) + 1]; /* Move to next line */
	}
	return -EINVAL; /* No CGDCONT found */
}

int hio_lte_flow_open_socket(void)
{
	int ret;
	char cops[32] = {0};
	ret = hio_lte_talk_at_cops_q(cops, sizeof(cops));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cops_q` failed: %d", ret);
		return ret;
	}
	LOG_INF("COPS: %s", cops);

	/* Debugging AT commands */
	hio_lte_talk_at_cmd("AT+CEREG?");
	hio_lte_talk_at_cmd("AT%XCBAND");
	hio_lte_talk_at_cmd("AT+CEINFO?");
	hio_lte_talk_at_cmd("AT+CGPADDR=0");
	hio_lte_talk_at_cmd("AT+CGATT?");
	hio_lte_talk_at_cmd("AT+CGACT?");

	ret = update_cgdcont();
	if (ret) {
		LOG_ERR("Call `update_cgdcont_param` failed: %d", ret);
		return ret;
	}

	LOG_INF("addr: %s, port: %d", m_cgdcont.addr, CONFIG_HIO_LTE_PORT);

	k_mutex_lock(&m_addr_info_lock, K_FOREVER);
	m_addr_info.sin_family = NRF_AF_INET;
	m_addr_info.sin_port = nrf_htons(CONFIG_HIO_LTE_PORT);
	if (nrf_inet_pton(m_addr_info.sin_family, g_hio_lte_config.addr, &m_addr_info.sin_addr) <=
	    0) {
		LOG_ERR("Invalid IP address: %s", g_hio_lte_config.addr);
		k_mutex_unlock(&m_addr_info_lock);
		return -EINVAL;
	}
	k_mutex_unlock(&m_addr_info_lock);

	ret = nrf_socket(m_addr_info.sin_family, NRF_SOCK_DGRAM, NRF_IPPROTO_UDP);
	if (ret == -1) {
		ret = -errno;
		LOG_ERR("Call `nrf_socket` failed: %d", ret);
	}

	m_socket_fd = ret;

	struct nrf_timeval tv = {
		.tv_sec = SOCKET_SEND_TMO_SEC,
		.tv_usec = 0,
	};

	ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_SNDTIMEO, (const void *)&tv,
			     sizeof(struct nrf_timeval));
	if (ret < 0) {
		LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
		nrf_close(m_socket_fd);
		m_socket_fd = -1;
		return ret;
	}

	tv.tv_sec = RESPONSE_TIMEOUT_SEC;
	tv.tv_usec = 0;

	ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RCVTIMEO, (const void *)&tv,
			     sizeof(struct nrf_timeval));
	if (ret < 0) {
		LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
		nrf_close(m_socket_fd);
		m_socket_fd = -1;
		return ret;
	}

	if (m_cgdcont.cid > 0) {
		/* Bind socket to the specified PDN context ID */
		nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_BINDTOPDN, &m_cgdcont.cid,
			       sizeof(m_cgdcont.cid));
	}

	LOG_INF("Socket opened: %d", m_socket_fd);

	ret = nrf_connect(m_socket_fd, (struct nrf_sockaddr *)&m_addr_info, sizeof(m_addr_info));
	if (ret == -1) {
		ret = -errno;
		if (ret == -NRF_EINPROGRESS) {
			LOG_INF("Socket connecting: %d", m_socket_fd);
			k_sleep(K_SECONDS(30));
			return 0;
		} else if (ret != -NRF_EINPROGRESS) {
			LOG_ERR("Call `nrf_connect` failed: %d", ret);
			nrf_close(m_socket_fd);
			m_socket_fd = -1;
			return ret;
		}
	}

	LOG_INF("Socket connected");
	return 0;
}

int hio_lte_flow_check(void)
{
	int ret;

	char resp[128] = {0};

	/* Check functional mode */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CFUN?", resp, sizeof(resp), "+CFUN: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CFUN? failed: %d", ret);
		return ret;
	}

	if (strcmp(resp, "1") != 0) {
		LOG_ERR("Unexpected CFUN response: %s", resp);
		return -HIO_LTE_ERR_MODEM_INACTIVE;
	}

	/* Check network registration status */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CEREG?", resp, sizeof(resp), "+CEREG: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CEREG? failed: %d", ret);
		return ret;
	}

	if (resp[0] == '0') {
		LOG_ERR("CEREG unsubscribe unsolicited result codes");
		return -HIO_LTE_ERR_CEREG_NOT_SUBSCRIBED;
	}

	struct hio_lte_cereg_param cereg_param;

	ret = parse_cereg(resp + 2, &cereg_param);
	if (ret) {
		LOG_WRN("Call `hio_lte_parse_urc_cereg` failed: %d", ret);
		return ret;
	}

	hio_lte_state_set_cereg_param(&cereg_param);

	if (cereg_param.stat != HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME &&
	    cereg_param.stat != HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING) {
		LOG_ERR("Unexpected CEREG response: %s", resp);
		return -HIO_LTE_ERR_CEREG_NOT_REGISTERED;
	}

	/* Check if PDN is active */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CGATT?", resp, sizeof(resp), "+CGATT: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CGATT? failed: %d", ret);
		return ret;
	}

	if (strcmp(resp, "1") != 0) {
		LOG_ERR("Unexpected CGATT response: %s", resp);
		return -HIO_LTE_ERR_PDN_NOT_ATTACHED;
	}

	/* Check PDN connections */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CGACT?", resp, sizeof(resp), "+CGACT: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CGACT? failed: %d", ret);
		return ret;
	}

	if (strcmp(resp, "0,1") != 0) {
		LOG_ERR("Unexpected CGACT response: %s", resp);
		return -HIO_LTE_ERR_PDN_NOT_ACTIVE;
	}

	hio_lte_talk_at_cmd("AT+CGPADDR=0");

	int error;
	nrf_socklen_t len = sizeof(error);
	ret = nrf_getsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_ERROR, &error, &len);
	if (ret != 0 || error != 0) {
		return -HIO_LTE_ERR_SOCKET_ERROR;
	}

	return 0;
}

int hio_lte_flow_send(const struct hio_lte_send_recv_param *param)
{
	int ret;

	int option;
	if (param->rai) {
		option = param->recv_buf ? NRF_RAI_ONE_RESP : NRF_RAI_LAST;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RAI, &option,
				     sizeof(option));
		if (ret) {
			LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
			return ret;
		}
	}

	ssize_t total_sentb = 0;
	while (total_sentb < param->send_len) {
		ssize_t sentb = nrf_send(
			m_socket_fd, (const void *)((const uint8_t *)param->send_buf + total_sentb),
			param->send_len - total_sentb, 0);
		if (sentb == -1) {
			ret = -errno;
			if (ret == -NRF_EAGAIN) {
				LOG_ERR("Connection closed by the peer");
				return -ENOTCONN;
			}

			LOG_ERR("Failed to send data: %d", ret);
			return ret;
		}

		total_sentb += sentb;
	}

	if (param->rai && !param->recv_buf) {
		option = NRF_RAI_NO_DATA;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RAI, &option,
				     sizeof(option));
		if (ret) {
			LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
			return ret;
		}
	}

	LOG_INF("Sent %d bytes", total_sentb);

	return total_sentb;
}

int hio_lte_flow_recv(const struct hio_lte_send_recv_param *param)
{
	int ret;

	if (!param->recv_buf || !param->recv_size || !param->recv_len) {
		return -EINVAL;
	}

	struct nrf_timeval tv = {
		.tv_sec = RESPONSE_TIMEOUT_SEC,
		.tv_usec = 0,
	};

	ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RCVTIMEO, (const void *)&tv,
			     sizeof(struct nrf_timeval));
	if (ret < 0) {
		LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
		nrf_close(m_socket_fd);
		m_socket_fd = -1;
		return ret;
	}

	LOG_INF("Receiving data from socket_fd %d, expecting up to %u bytes", m_socket_fd,
		param->recv_size);

	ssize_t readb =
		nrf_recv(m_socket_fd, (void *)((uint8_t *)param->recv_buf + *param->recv_len),
			 param->recv_size - *param->recv_len, 0);
	if (readb < 0) {
		ret = -errno;

		if (ret == -NRF_EAGAIN) {
			LOG_ERR("Receive operation timed out");
			return -ETIMEDOUT;
		} else if (ret == -NRF_ECONNREFUSED) {
			LOG_ERR("Connection refused");
			return -ECONNREFUSED;
		} else {
			LOG_ERR("Failed to receive data: %d", ret);
			return ret;
		}
	} else if (readb == 0) {
		// Remote side has closed the connection (EOF)
		LOG_ERR("Connection closed by the peer");
		return -ENOTCONN;
	} else {
		LOG_INF("Received %zd bytes", readb);
		*param->recv_len += readb;

		if (*param->recv_len >= param->recv_size) {
			LOG_INF("Received all expected data");
		}
	}

	if (param->rai) {
		int option = NRF_RAI_NO_DATA;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RAI, &option,
				     sizeof(option));
		if (ret) {
			LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
		}
	}

	return readb;
}

int parse_coneval(const char *str, struct hio_lte_conn_param *params)
{
	int ret;

	if (!str || !params) {
		return -EINVAL;
	}

	/* 0,1,5,8,2,14,"011B0780”,"26295",7,1575,3,1,1,23,16,32,130 */
	/* r,-,e,r,r,s ,"CIDCIDCI","PLMNI",f,g   ,h,i,j,k ,l ,m ,n  */
	/* 0,1,9,72,22,47,"00094F0C","26806",382,6200,20,0,0,-8,1,1,87*/
	memset(params, 0, sizeof(*params));

	int result;
	int energy_estimate;
	int rsrp;
	int rsrq;
	int snr;
	char cid[8 + 1];
	int plmn;
	int earfcn;
	int band;
	int ce_level;

	ret = sscanf(str, "%d,%*d,%d,%d,%d,%d,\"%8[0-9A-F]\",\"%d\",%*d,%d,%d,%*d,%d", &result,
		     &energy_estimate, &rsrp, &rsrq, &snr, cid, &plmn, &earfcn, &band, &ce_level);
	if (ret != 1 && ret != 10) {
		LOG_ERR("Failed to parse coneval");
		return -EINVAL;
	}

	params->result = result;

	if (params->result != 0) {
		return 0;
	}

	if (cellid_hex2int(cid, sizeof(cid), &params->cid) != 0) {
		return -EINVAL;
	}

	params->eest = energy_estimate;
	params->rsrp = rsrp - 140;
	params->rsrq = (rsrq - 39) / 2;
	params->snr = snr - 24;
	params->plmn = plmn;
	params->earfcn = earfcn;
	params->band = band;
	params->ecl = ce_level;
	params->valid = true;

	return 0;
}

int hio_lte_flow_coneval(void)
{
	int ret;

	char buf[128] = {0};

	ret = hio_lte_talk_at_coneval(buf, sizeof(buf));
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_coneval` failed: %d", ret);
		return ret;
	}

	struct hio_lte_conn_param conn_params;

	ret = parse_coneval(buf, &conn_params);
	if (ret) {
		LOG_ERR("Failed to parse coneval: %d", ret);
		return ret;
	}

	if (conn_params.result != 0) {
		LOG_ERR("Connection evaluation: %s",
			hio_lte_coneval_result_str(conn_params.result));
		return -EIO;
	}

	hio_lte_state_set_conn_param(&conn_params);

	return 0;
}

int hio_lte_flow_cmd(const char *s)
{
	int ret;

	if (!s) {
		return -EINVAL;
	}

	if (!nrf_modem_is_initialized()) {
		return -ENOTCONN;
	}

	ret = hio_lte_talk_(s);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_flow_cmd_test_cmd(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc > 2) {
		shell_error(shell, "only one argument is accepted (use quotes?)");
		shell_help(shell);
		return -EINVAL;
	}

	if (!g_hio_lte_config.test) {
		shell_error(shell, "test mode is not activated");
		return -ENOEXEC;
	}

	char buf[128] = {0};
	ret = hio_lte_talk_at_cmd_with_resp(argv[1], buf, sizeof(buf));
	if (!ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	if (buf[0]) {
		shell_print(shell, "%s", buf);
	}

	shell_print(shell, "command succeeded");

	return 0;
}

int hio_lte_flow_cmd_trace(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = hio_lte_talk_at_xmodemtrace(5);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xmodemtrace` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_flow_init(hio_lte_flow_event_delegate_cb cb)
{
	m_event_delegate_cb = cb;

	int ret = nrf_modem_lib_init();
	if (ret) {
		LOG_ERR("Call `nrf_modem_lib_init` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_cfun(0);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cfun: 0` failed: %d", ret);
		return ret;
	}

	ret = hio_lte_talk_at_xmodemtrace(g_hio_lte_config.modemtrace ? 2 : 0);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_xmodemtrace` failed: %d", ret);
		return ret;
	}

	return 0;
}
