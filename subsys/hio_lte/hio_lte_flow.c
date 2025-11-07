#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_parse.h"
#include "hio_lte_state.h"
#include "hio_lte_str.h"
#include "hio_lte_talk.h"

/* HIO includes */
#include <hio/hio_rtc.h>
#include <hio/hio_lte.h>
#include <hio/hio_tok.h>

/* nRF includes */
#include <modem/nrf_modem_lib.h>
#include <modem/at_monitor.h>
#include <modem/at_parser.h>
#include <nrf_modem_at.h>
#include <nrf_socket.h>
#include <nrf_errno.h>

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

#define XSLEEP_PAUSE K_MSEC(100)

#define XRECVFROM_TIMEOUT_SEC 5

#define SOCKET_SEND_TMO_SEC  30
#define RESPONSE_TIMEOUT_SEC 5

static K_EVENT_DEFINE(m_flow_events);

static HIO_LTE_FSM_EVENT_delegate_cb m_event_delegate_cb;

static K_MUTEX_DEFINE(m_addr_info_lock);
static struct nrf_sockaddr_in m_addr_info;
static struct cgdcont_param m_cgdcont;

static int m_socket_fd = -1;

static void process_urc(const char *line, void *user_data)
{
	int ret;
	ARG_UNUSED(user_data);

	if (!line) {
		LOG_ERR("URC line is NULL");
		return;
	}

	if (g_hio_lte_config.test) {
		return; /* Test mode active, ignoring URC */
	}

	LOG_INF("URC: %s", line);

	if (!strcmp(line, "Ready")) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_READY);
	} else if (!strncmp(line, "%XSIM: 1", 8)) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_SIMDETECTED);
	} else if (!strncmp(line, "%XTIME:", 7)) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_XTIME);
	} else if (!strncmp(line, "+CEREG: ", 8)) {
		struct hio_lte_cereg_param cereg_param = {0};

		ret = hio_lte_parse_urc_cereg(&line[8], &cereg_param);
		if (ret) {
			LOG_WRN("Call `hio_lte_parse_urc_cereg` failed: %d", ret);
			return;
		}

		if (!cereg_param.valid) {
			LOG_WRN("CEREG was %d\n", (enum hio_lte_cereg_param_stat)cereg_param.stat);
			return;
		}

		hio_lte_state_set_cereg_param(&cereg_param);

		if (cereg_param.stat == HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME ||
		    cereg_param.stat == HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING) {
			m_event_delegate_cb(HIO_LTE_FSM_EVENT_REGISTERED);
		} else {
			m_event_delegate_cb(HIO_LTE_FSM_EVENT_DEREGISTERED);
		}
	} else if (!strncmp(line, "%MDMEV: ", 8)) {
		if (!strncmp(&line[8], "RESET LOOP", 10)) {
			LOG_WRN("Modem reset loop detected");
			m_event_delegate_cb(HIO_LTE_FSM_EVENT_RESET_LOOP);
		}
	} else if (!strncmp(line, "+CSCON: 0", 9)) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_CSCON_0);
	} else if (!strncmp(line, "+CSCON: 1", 9)) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_CSCON_1);
	} else if (!strncmp(line, "%XMODEMSLEEP: ", 14)) {
		int p1 = 0, p2 = 0;

		ret = hio_lte_parse_urc_xmodemsleep(line + 14, &p1, &p2);
		if (ret) {
			LOG_WRN("Call `hio_lte_parse_urc_xmodemsleep` failed: %d", ret);
			return;
		}
		if (p2 > 0 || p1 == 4) {
			m_event_delegate_cb(HIO_LTE_FSM_EVENT_XMODEMSLEEP);
		}
	} else if (!strncmp(line, "%RAI: ", 6)) {
		struct hio_lte_rai_param rai_param = {0};
		ret = hio_lte_parse_urc_rai(line + 6, &rai_param);
		if (ret) {
			LOG_WRN("Call `hio_lte_parse_urc_rai` failed: %d", ret);
			return;
		}

		hio_lte_state_set_rai_param(&rai_param);
	} else if (!strncmp(line, "%NCELLMEAS: ", 12)) {
		struct hio_lte_ncellmeas_param ncellmeas_param = {0};
		ret = hio_lte_parse_urc_ncellmeas(line + 12, 5, &ncellmeas_param);
		if (ret) {
			LOG_WRN("Call `hio_lte_parse_urc_ncellmeas` failed: %d", ret);
			return;
		}
		if (ncellmeas_param.valid) {
			LOG_INF("NCELLMEAS: %d cells, %d ncells", ncellmeas_param.num_cells,
				ncellmeas_param.num_ncells);
			m_event_delegate_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
			hio_lte_state_set_ncellmeas_param(&ncellmeas_param);
		}
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
		if (!(p = hio_tok_num(p, &def, &band)) || !def || band < 0 || band > 255) {
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

		if (hio_tok_end(p)) {
			break;
		}

		if (!(p = hio_tok_sep(p))) {
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

	ret = hio_lte_talk_at_cmd("AT%XEPCO=0");
	if (ret) {
		LOG_ERR("Call `AT%%XEPCO=0` failed: %d", ret);
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
		ret = hio_lte_parse_cgcont(line, &m_cgdcont);
		if (ret) {
			LOG_ERR("Call `hio_lte_parse_cgcont` failed: %d", ret);
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
	hio_lte_talk_at_cmd("AT+CGATT?");
	hio_lte_talk_at_cmd("AT+CGACT?");

	ret = update_cgdcont();
	if (ret) {
		LOG_ERR("Call `update_cgdcont_param` failed: %d", ret);
		return ret;
	}

	LOG_INF("addr: %s, port: %d", g_hio_lte_config.addr, CONFIG_HIO_LTE_PORT);

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

	if (m_socket_fd >= 0) {
		LOG_INF("Closing existing socket: %d", m_socket_fd);
		nrf_close(m_socket_fd);
		m_socket_fd = -1;
	}

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

	// nrf_close(m_socket_fd);
	// m_socket_fd = -1;
	// return -114;

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
		return -ENODEV;
	}

	/* Check network registration status */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CEREG?", resp, sizeof(resp), "+CEREG: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CEREG? failed: %d", ret);
		return ret;
	}

	if (resp[0] == '0') {
		LOG_ERR("CEREG unsubscribe unsolicited result codes");
		return -EOPNOTSUPP;
	}

	struct hio_lte_cereg_param cereg_param;

	ret = hio_lte_parse_urc_cereg(resp + 2, &cereg_param);
	if (ret) {
		LOG_WRN("Call `hio_lte_parse_urc_cereg` failed: %d", ret);
		return ret;
	}

	hio_lte_state_set_cereg_param(&cereg_param);

	if (cereg_param.stat != HIO_LTE_CEREG_PARAM_STAT_REGISTERED_HOME &&
	    cereg_param.stat != HIO_LTE_CEREG_PARAM_STAT_REGISTERED_ROAMING) {
		LOG_ERR("Unexpected CEREG response: %s", resp);
		return -ENETUNREACH;
	}

	/* Check if PDN is active */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CGATT?", resp, sizeof(resp), "+CGATT: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CGATT? failed: %d", ret);
		return ret;
	}

	if (strcmp(resp, "1") != 0) {
		LOG_ERR("Unexpected CGATT response: %s", resp);
		return -ENETDOWN;
	}

	/* Check PDN connections */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CGACT?", resp, sizeof(resp), "+CGACT: ");
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmd_with_resp_prefix` AT+CGACT? failed: %d", ret);
		return ret;
	}

	if (strcmp(resp, "0,1") != 0) {
		LOG_ERR("Unexpected CGACT response: %s", resp);
		return -ENOTCONN;
	}

	hio_lte_talk_at_cmd("AT+CGPADDR=0");

	if (m_socket_fd < 0) {
		LOG_ERR("Socket is not opened");
		return -ENOTSOCK;
	}

	int error;
	nrf_socklen_t len = sizeof(error);
	ret = nrf_getsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_ERROR, &error, &len);
	if (ret != 0 || error != 0) {
		LOG_ERR("Socket error: %d (ret %d)", error, ret);
		return -ENOTSOCK;
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

	ret = hio_lte_parse_coneval(buf, &conn_params);
	if (ret) {
		LOG_ERR("Failed to parse coneval: %d", ret);
		return ret;
	}

	if (conn_params.result != 0) {
		LOG_ERR("Connection evaluation: %s",
			hio_lte_str_coneval_result(conn_params.result));
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

	ret = hio_lte_talk_at_cmd(s);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_flow_xmodemtrace(int lvl)
{
	int ret;

	if (!nrf_modem_is_initialized()) {
		return -ENOTCONN;
	}

	ret = hio_lte_talk_at_xmodemtrace(lvl);
	if (ret < 0) {
		LOG_ERR("Call `hio_lte_talk_at_xmodemtrace` failed: %d", ret);
		return ret;
	}

	return 0;
}

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_periodic(int attempt, k_timeout_t pause)
{
	switch (attempt % 3) {
	case 0:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_NO_WAIT};
	case 1:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_NO_WAIT};
	default: /* 2 */
		return (struct hio_lte_attach_timeout){K_MINUTES(50), pause};
	}
}

struct hio_lte_attach_timeout hio_lte_flow_attach_policy_progressive(int attempt)
{
	switch (attempt) {
	case 0:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_NO_WAIT};
	case 1:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_NO_WAIT};
	case 2:
		return (struct hio_lte_attach_timeout){K_MINUTES(50), K_HOURS(1)};
	case 3:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_MINUTES(5)};
	case 4:
		return (struct hio_lte_attach_timeout){K_MINUTES(45), K_HOURS(6)};
	case 5:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_MINUTES(5)};
	case 6:
		return (struct hio_lte_attach_timeout){K_MINUTES(45), K_HOURS(24)};
	case 7:
		return (struct hio_lte_attach_timeout){K_MINUTES(5), K_MINUTES(5)};
	case 8:
		return (struct hio_lte_attach_timeout){K_MINUTES(45), K_HOURS(168)};
	default: {
		/* 9+: attach alternates 5m (odd), 45m (even) */
		k_timeout_t attach = (attempt & 1) ? K_MINUTES(5) : K_MINUTES(45);
		// delay is determined by the NEXT attempt: for next=odd => 168h, otherwise 5m
		k_timeout_t delay = ((attempt + 1) & 1) ? K_HOURS(168) : K_MINUTES(5);
		return (struct hio_lte_attach_timeout){attach, delay};
	}
	};
}

int hio_lte_flow_init(HIO_LTE_FSM_EVENT_delegate_cb cb)
{
	m_socket_fd = -1;

	m_event_delegate_cb = cb;

	hio_lte_talk_init(process_urc, NULL);

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
