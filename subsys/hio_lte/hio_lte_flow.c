#include "hio_lte_config.h"
#include "hio_lte_flow.h"
#include "hio_lte_parse.h"
#include "hio_lte_state.h"
#include "hio_lte_str.h"
#include "hio_lte_talk.h"
#include "hio_lte_util.h"

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
#include <zephyr/net/socket.h>
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

/* SNDTIMEO bounds nrf_send with NRF_MSG_WAITACK, which blocks until the data is
 * delivered on-air. Because the modem only attempts a connection (CSCON 1) in
 * response to a send, this wait spans the entire time-to-CSCON-1, so it MUST be
 * at least as long as SEND_CSCON_1_TIMEOUT — otherwise the send is aborted with
 * ETIMEDOUT before the network is given its full chance to set up the bearer,
 * the FSM's longer CSCON timeout never gets reached, and the transfer stalls.
 * Field test with SNDTIMEO=30 against CSCON timeout=120 failed exactly this way.
 * Keep these two values in lockstep. */
#define SOCKET_SEND_TMO_SEC  120
#define RESPONSE_TIMEOUT_SEC 5

#define SEC_TAG 5005

static K_EVENT_DEFINE(m_flow_events);

static HIO_LTE_FSM_EVENT_delegate_cb m_event_delegate_cb;

static struct nrf_sockaddr_in m_addr_info;
static struct cgdcont_param m_cgdcont;

static int m_socket_fd = -1;

/* Scan result being assembled; published by hio_lte_flow_scan_end(). */
static struct hio_lte_scan_result m_scan;
static K_SEM_DEFINE(m_scan_cells_sem, 0, 1);
static K_SEM_DEFINE(m_scan_plmn_sem, 0, 1);
static bool m_scan_cells_pending;
static bool m_scan_plmn_pending;

static void process_urc_ncellmeas(const char *line)
{
	struct hio_lte_ncellmeas_param ncellmeas_param = {0};

	int ret = hio_lte_parse_urc_ncellmeas(line, 5, &ncellmeas_param);
	if (ret) {
		LOG_WRN("Call `hio_lte_parse_urc_ncellmeas` failed: %d", ret);
	}

	if (ncellmeas_param.valid) {
		LOG_INF("NCELLMEAS: %d cells, %d ncells", ncellmeas_param.num_cells,
			ncellmeas_param.num_ncells);
		hio_lte_state_set_ncellmeas_param(&ncellmeas_param);

		if (m_scan_cells_pending) {
			m_scan.cells_status = 0;
			m_scan.cell_count = MIN(ncellmeas_param.num_cells, ARRAY_SIZE(m_scan.cells));
			memcpy(m_scan.cells, ncellmeas_param.cells,
			       m_scan.cell_count * sizeof(m_scan.cells[0]));
			for (size_t i = 0; i < m_scan.cell_count; i++) {
				m_scan.cells[i].ncells = NULL;
			}
		}
	} else {
		LOG_WRN("NCELLMEAS data not valid");
		if (m_scan_cells_pending) {
			m_scan.cells_status = -EIO;
		}
	}

	/* Invalid data only ends a scan step; NCELLMEAS state keeps waiting. */
	bool notify = ncellmeas_param.valid || m_scan_cells_pending;

	m_scan_cells_pending = false;
	k_sem_give(&m_scan_cells_sem);
	if (notify && !g_hio_lte_config.test) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	}
}

static void log_emm_cause(const char *what, int cause)
{
	const char *hint = hio_lte_str_emm_cause_hint(cause);

	LOG_WRN("%s: EMM cause %d (%s)%s%s", what, cause, hio_lte_str_emm_cause(cause),
		hint ? ": " : "", hint ? hint : "");
}

static void log_esm_cause(int cause)
{
	const char *hint = hio_lte_str_esm_cause_hint(cause);

	LOG_WRN("ESM cause %d (%s)%s%s", cause, hio_lte_str_esm_cause(cause), hint ? ": " : "",
		hint ? hint : "");
}

static void process_urc(const char *line, void *user_data)
{
	int ret;
	ARG_UNUSED(user_data);

	if (!line) {
		LOG_ERR("URC line is NULL");
		return;
	}

	if (g_hio_lte_config.test) {
		/* Test mode: only feed 'lte scan', never the FSM. */
		if (!strncmp(line, "%NCELLMEAS: ", 12)) {
			process_urc_ncellmeas(line + 12);
		}
		return;
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
		hio_lte_state_add_cereg_event(&cereg_param);

		/* cause_type 0 is an EMM cause, 1 is manufacturer specific. */
		if (cereg_param.cause_type == 0 && cereg_param.reject_cause) {
			char what[48];

			snprintf(what, sizeof(what), "Registration rejected, tac %s, cell %08X",
				 cereg_param.tac, cereg_param.cid);
			log_emm_cause(what, cereg_param.reject_cause);
		}

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
		process_urc_ncellmeas(line + 12);
	} else if (!strncmp(line, "+CNEC_EMM: ", 11)) {
		log_emm_cause("Network", atoi(line + 11));
	} else if (!strncmp(line, "+CNEC_ESM: ", 11)) {
		log_esm_cause(atoi(line + 11));
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
		char bands[] = "00000000000000000000000000000000000000000000000000000000000"
			       "000000000"
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

	ret = hio_lte_talk_at_powerclass(
		g_hio_lte_config.powerclass == HIO_LTE_CONFIG_POWERCLASS_23_DBM ? 3 : 5);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_powerclass` failed: %d", ret);
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

static int socket_setup(bool dtls_enabled, bool load_dtls_session)
{
	int ret;

	/* Both SNDTIMEO and RCVTIMEO here are just initial defaults: the FSM
	 * overrides SNDTIMEO per send (hio_lte_flow_set_sndtimeo, from the
	 * caller's remaining deadline) and RCVTIMEO per receive (hio_lte_flow_recv,
	 * from the granted PSM active time). nrf_send uses NRF_MSG_WAITACK and
	 * blocks until the data is on-air, so SNDTIMEO is the real bound on the
	 * time-to-CSCON-1 wait, not a mere full-TX-buffer guard. */
	struct nrf_timeval tv = {
		.tv_sec = SOCKET_SEND_TMO_SEC,
		.tv_usec = 0,
	};

	ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_SNDTIMEO, (const void *)&tv,
			     sizeof(struct nrf_timeval));
	if (ret < 0) {
		LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
		return ret;
	}

	tv.tv_sec = RESPONSE_TIMEOUT_SEC;
	tv.tv_usec = 0;

	ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RCVTIMEO, (const void *)&tv,
			     sizeof(struct nrf_timeval));
	if (ret < 0) {
		LOG_ERR("Call `nrf_setsockopt` failed: %d", ret);
		return ret;
	}

	/* Bind socket to the specified PDN context ID */
	if (m_cgdcont.cid > 0) {
		LOG_INF("Binding to PDN context ID: %d", m_cgdcont.cid);
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_BINDTOPDN, &m_cgdcont.cid,
				     sizeof(m_cgdcont.cid));
		if (ret < 0) {
			LOG_ERR("Set BINDTOPDN failed: %d", -ret);
			return ret;
		}
	}

	if (dtls_enabled) {
		/* Set up DTLS security tag */
		nrf_sec_tag_t sec_tags[] = {SEC_TAG};
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_TAG_LIST, sec_tags,
				     sizeof(sec_tags));
		if (ret) {
			LOG_ERR("Set SEC_TAG_LIST failed: %d", -ret);
			return ret;
		}

		/* Enable DTLS connection ID if configured */
		int cid_option = NRF_SO_SEC_DTLS_CID_SUPPORTED;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_DTLS_CID, &cid_option,
				     sizeof(cid_option));
		if (ret) {
			LOG_ERR("Set SEC_DTLS_CID failed: %d", -ret);
			return ret;
		}

		/* Set DTLS handshake timeout */
		int timeout = TLS_DTLS_HANDSHAKE_TIMEO_7S;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_DTLS_HANDSHAKE_TIMEO,
				     &timeout, sizeof(timeout));
		if (ret) {
			LOG_ERR("Set SEC_DTLS_HANDSHAKE_TIMEO failed: %d", -ret);
			return ret;
		}

		/* Set up peer verification */
		int verify = NRF_SO_SEC_PEER_VERIFY_REQUIRED;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_PEER_VERIFY, &verify,
				     sizeof(verify));

		if (ret) {
			LOG_ERR("Set SEC_PEER_VERIFY failed: %d", ret);
			return ret;
		}

		/* Enable session caching */
		int session_cache = NRF_SO_SEC_SESSION_CACHE_ENABLED;
		ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_SESSION_CACHE,
				     &session_cache, sizeof(session_cache));
		if (ret) {
			LOG_ERR("Set SEC_SESSION_CACHE failed: %d", ret);
			return ret;
		}

		if (load_dtls_session) {
			/* Load saved DTLS session */
			int load_dtls = 1;
			ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_DTLS_CONN_LOAD,
					     &load_dtls, sizeof(load_dtls));
			if (ret) {
				int err = errno;
				LOG_WRN("Set SEC_DTLS_CONN_LOAD failed: %d (errno %d)", ret, err);
			} else {
				LOG_INF("DTLS session restored");
			}
		}
	}

	return 0;
}

static int open_socket(const struct hio_lte_socket_config *socket_config, bool load_dtls_session)
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

	int protocol = NRF_IPPROTO_UDP;
	if (socket_config->dtls_enabled) {
		protocol = NRF_SPROTO_DTLS1v2;
	}

	m_addr_info.sin_family = NRF_AF_INET;
	m_addr_info.sin_port = nrf_htons(socket_config->port);
	if (nrf_inet_pton(m_addr_info.sin_family, socket_config->addr, &m_addr_info.sin_addr) <=
	    0) {
		LOG_ERR("Invalid IP address: %s", socket_config->addr);
		return -EINVAL;
	}

	if (m_socket_fd >= 0) {
		// NRF_SO_SEC_SESSION_CACHE_PURGE
		LOG_INF("Closing existing socket: %d", m_socket_fd);
		nrf_close(m_socket_fd);
		m_socket_fd = -1;
	}

	ret = nrf_socket(m_addr_info.sin_family, NRF_SOCK_DGRAM, protocol);
	if (ret == -1) {
		LOG_ERR("Call `nrf_socket` failed: %d", ret);
		ret = -errno;
	}

	m_socket_fd = ret;

	ret = socket_setup(socket_config->dtls_enabled, load_dtls_session);
	if (ret < 0) {
		nrf_close(m_socket_fd);
		m_socket_fd = -1;
		LOG_ERR("Creating socket failed: %d", ret);
		return ret;
	}

	LOG_INF("Connection to addr: %s, port: %d", socket_config->addr, socket_config->port);

	ret = nrf_connect(m_socket_fd, (struct nrf_sockaddr *)&m_addr_info, sizeof(m_addr_info));
	if (ret == -1) {
		ret = -errno;
		if (ret == -NRF_EINPROGRESS) {
			LOG_INF("Connecting: %d", m_socket_fd);
			k_sleep(K_SECONDS(30));
			return 0;
		} else if (ret != -NRF_EINPROGRESS) {
			LOG_ERR("Call `nrf_connect` failed: %d", ret);
			nrf_close(m_socket_fd);
			m_socket_fd = -1;
			return ret;
		}
	}

	int ciph = 0;
	if (socket_config->dtls_enabled) {
		/* Get Cipher */
		nrf_socklen_t optlen = sizeof(ciph);
		int ret = nrf_getsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_CIPHERSUITE_USED,
					 &ciph, &optlen);
		LOG_INF("DTLS Cipher suite used: 0x%04x %s (ret %d)", ciph,
			hio_lte_str_ciphersuite(ciph), ret);

		int cid_status;
		optlen = sizeof(cid_status);
		ret = nrf_getsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_DTLS_CID_STATUS,
				     &cid_status, &optlen);
		LOG_INF("DTLS CID status: %d (ret %d)", cid_status, ret);
	}
	hio_lte_state_set_dtls_ciphersuite_used(ciph);

	LOG_INF("Connected");
	return 0;
}

int hio_lte_flow_open_socket(const struct hio_lte_socket_config *socket_config,
			     bool load_dtls_session)
{
	return open_socket(socket_config, load_dtls_session);
}

static int close_socket(bool save_dtls_session)
{
	if (m_socket_fd < 0) {
		LOG_DBG("Socket is closed");
		return 0;
	}
	if (save_dtls_session) {
		int store_dtls = 1;
		int ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SECURE, NRF_SO_SEC_DTLS_CONN_SAVE,
					 &store_dtls, sizeof(store_dtls));
		if (ret) {
			LOG_WRN("Set SEC_DTLS_CONN_SAVE failed: %d", ret);
		} else {
			LOG_INF("DTLS session saved");
			k_sleep(K_MSEC(1000));
		}
	}
	// if (m_socket_fd >= 0) {
	// 	nrf_close(m_socket_fd); /* this send close notify for DTLS */
	// 	m_socket_fd = -1;
	// }
	return 0;
}

int hio_lte_flow_close_socket(bool save_dtls_session)
{
	return close_socket(save_dtls_session);
}

int hio_lte_flow_check(void)
{
	int ret;

	char resp[128] = {0};

	/* Best-effort: capture the last network release/reject cause so field
	 * failures (e.g. RRC release with extended wait time) are visible in
	 * the log and via `lte state`. */
	ret = hio_lte_talk_at_cmd_with_resp_prefix("AT+CEER", resp, sizeof(resp), "+CEER: ");
	if (!ret && resp[0] != '\0') {
		LOG_WRN("CEER: %s", resp);
		hio_lte_state_set_ceer(resp);
	}

	/* Best-effort diagnostics for the China low-throughput case: whether the
	 * network imposes APN rate control (rate / time_window / remaining block
	 * time) and the granted eDRX cycle. Raw queries — the full modem
	 * response is logged via tx:/rx:, no parsing needed yet. */
	hio_lte_talk_at_cmd("AT%APNRATECTRL=0,0");
	hio_lte_talk_at_cmd("AT+CEDRXRDP");

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

int hio_lte_flow_set_sndtimeo(int timeout_sec)
{
	struct nrf_timeval tv = {
		.tv_sec = timeout_sec,
		.tv_usec = 0,
	};

	int ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_SNDTIMEO, (const void *)&tv,
				 sizeof(struct nrf_timeval));
	if (ret < 0) {
		LOG_ERR("Call `nrf_setsockopt` SNDTIMEO failed: %d", -ret);
		return ret;
	}

	return 0;
}

int hio_lte_flow_send(const struct hio_lte_send_recv_param *param)
{
	int ret;

	/* Always rewrite the RAI option: a value armed by a previous exchange
	 * (ONE_RESP/LAST) would otherwise stick to the socket and make the
	 * network drop RRC right after every mid-transfer fragment, forcing a
	 * new RRC setup per fragment. ONGOING tells the modem to keep the
	 * connection while the transfer continues. */
	int option;
	if (param->rai) {
		option = param->recv_buf ? NRF_RAI_ONE_RESP : NRF_RAI_LAST;
	} else {
		option = NRF_RAI_ONGOING;
	}
	ret = nrf_setsockopt(m_socket_fd, NRF_SOL_SOCKET, NRF_SO_RAI, &option, sizeof(option));
	if (ret) {
		LOG_ERR("Call `nrf_setsockopt` failed: %d", -ret);
		return ret;
	}

	/* NRF_MSG_WAITACK blocks until the datagram is actually sent on-air by
	 * the modem (bounded by SNDTIMEO), instead of just being queued. This is
	 * what keeps the caller from handing the modem a second packet while the
	 * first is still buffered waiting for RRC: without RRC the modem queues
	 * sends and flushes them in one burst once a connection is granted, which
	 * the server then sees as a duplicate storm and resets the sequence.
	 * On SNDTIMEO the network never granted a connection in time -> ETIMEDOUT,
	 * which the FSM maps to "no connection" so the upper layer can back off.
	 *
	 * One datagram per call: on a UDP socket nrf_send transmits the whole
	 * datagram or fails, so a single send maps to a single FLAP packet on
	 * the wire.
	 *
	 * Validate against a snapshot, never by re-reading param: this call
	 * blocks for the whole time-to-CSCON-1, so the post-send check must
	 * compare against the length actually handed to the modem. */
	const size_t send_len = param->send_len;

	ssize_t sentb = nrf_send(m_socket_fd, param->send_buf, send_len, NRF_MSG_WAITACK);
	if (sentb == -1) {
		ret = -errno;
		if (ret == -NRF_EAGAIN) {
			LOG_WRN("Send not delivered on-air within timeout");
			return -ETIMEDOUT;
		}
		LOG_ERR("Failed to send data: %d", ret);
		return ret;
	}
	if (sentb != send_len) {
		LOG_ERR("Partial datagram send: %zd of %u", sentb, send_len);
		return -EIO;
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

	LOG_INF("Sent %zd bytes, rai: %d", sentb, param->rai);

	return sentb;
}

int hio_lte_flow_recv(const struct hio_lte_send_recv_param *param)
{
	int ret;

	if (!param->recv_buf || !param->recv_size || !param->recv_len) {
		return -EINVAL;
	}

	struct hio_lte_cereg_param cereg = {0};
	hio_lte_state_get_cereg_param(&cereg);

	struct nrf_timeval tv = {
		.tv_sec = hio_lte_util_recv_timeout_sec(&cereg),
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

	LOG_INF("Receiving data from socket_fd %d, expecting up to %u bytes, timeout %lld s",
		m_socket_fd, param->recv_size, (long long)tv.tv_sec);

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

static void scan_plmn_work_handler(struct k_work *work)
{
	LOG_INF("PLMN scan done: %d, networks: %u", m_scan.plmn_status, m_scan.count);

	m_scan_plmn_pending = false;

	k_sem_give(&m_scan_plmn_sem);
	m_event_delegate_cb(HIO_LTE_FSM_EVENT_COPS_DONE);
}

static K_WORK_DEFINE(m_scan_plmn_work, scan_plmn_work_handler);

/* ISR context: parse only, no locks. */
static void scan_plmn_resp_handler(const char *resp)
{
	size_t count = 0;
	int ret = 0;

	const char *list = strstr(resp, "%COPS: ");
	if (list) {
		ret = hio_lte_parse_cops_list(list + strlen("%COPS: "), m_scan.entries,
					      ARRAY_SIZE(m_scan.entries), &count);
	}
	m_scan.count = MIN(count, UINT8_MAX);

	const char *cme = strstr(resp, "+CME ERROR: ");
	if (cme) {
		int code = atoi(cme + strlen("+CME ERROR: "));
		m_scan.plmn_status = code == 521 ? -EINTR : code == 516 ? -EBUSY : -EIO;
	} else if (strstr(resp, "ERROR")) {
		m_scan.plmn_status = -EIO;
	} else {
		m_scan.plmn_status = list ? ret : -EBADMSG;
	}

	k_work_submit(&m_scan_plmn_work);
}

static void scan_cells_fail_work_handler(struct k_work *work)
{
	LOG_ERR("Cell search refused by the modem");

	m_scan_cells_pending = false;
	k_sem_give(&m_scan_cells_sem);
	if (!g_hio_lte_config.test) {
		m_event_delegate_cb(HIO_LTE_FSM_EVENT_NCELLMEAS);
	}
}

static K_WORK_DEFINE(m_scan_cells_fail_work, scan_cells_fail_work_handler);

/* ISR context. A refused %NCELLMEAS never sends the result URC. */
static void scan_cells_resp_handler(const char *resp)
{
	if (strstr(resp, "ERROR")) {
		m_scan.cells_status = -EIO;
		k_work_submit(&m_scan_cells_fail_work);
	}
}

void hio_lte_flow_scan_begin(enum hio_lte_scan_mode mode, bool auto_triggered)
{
	m_scan_cells_pending = false;
	m_scan_plmn_pending = false;
	memset(&m_scan, 0, sizeof(m_scan));
	m_scan.mode = mode;
	m_scan.auto_triggered = auto_triggered;
	m_scan.cells_status = -ENODATA;
	m_scan.plmn_status = -ENODATA;
}

void hio_lte_flow_scan_end(void)
{
	m_scan.valid = true;
	m_scan.uptime_ms = k_uptime_get();
	hio_lte_state_set_scan_result(&m_scan);

	LOG_INF("Scan done, cells: %d, networks: %d (%u)", m_scan.cells_status,
		m_scan.plmn_status, m_scan.count);
	for (size_t i = 0; i < MIN(m_scan.count, ARRAY_SIZE(m_scan.entries)); i++) {
		LOG_INF("plmn: %s, act: %u, stat: %u", m_scan.entries[i].plmn,
			m_scan.entries[i].act, m_scan.entries[i].stat);
	}
}

/* Result arrives as a %NCELLMEAS URC. */
int hio_lte_flow_scan_cells_start(void)
{
	int ret = -ENOTCONN;

	k_sem_reset(&m_scan_cells_sem);
	m_scan_cells_pending = true;

	if (nrf_modem_is_initialized()) {
		ret = hio_lte_talk_ncellmeas_cb(5, HIO_LTE_NCELLMEAS_CELL_MAX,
						scan_cells_resp_handler);
	}

	if (ret) {
		LOG_ERR("Cell search not started: %d", ret);
		m_scan_cells_pending = false;
		m_scan.cells_status = ret;
	}

	return ret;
}

int hio_lte_flow_scan_cells_wait(k_timeout_t timeout)
{
	return k_sem_take(&m_scan_cells_sem, timeout);
}

/* Always completes through COPS_DONE, also when it fails to start. */
int hio_lte_flow_scan_plmn_start(void)
{
	int ret = -ENOTCONN;

	k_sem_reset(&m_scan_plmn_sem);

	if (nrf_modem_is_initialized()) {
		ret = hio_lte_talk_at_pcops_list_async(scan_plmn_resp_handler);
		if (!ret) {
			m_scan_plmn_pending = true;
			return 0;
		}
	}

	LOG_ERR("PLMN scan not started: %d", ret);
	m_scan.plmn_status = ret;
	k_work_submit(&m_scan_plmn_work);

	return ret;
}

int hio_lte_flow_scan_plmn_wait(k_timeout_t timeout)
{
	return k_sem_take(&m_scan_plmn_sem, timeout);
}

/* The scan ends early (DISABLE, ERROR): stop the cell search and publish
 * what was collected. %COPS=? cannot be stopped. */
void hio_lte_flow_scan_abort(void)
{
	if (m_scan_cells_pending) {
		m_scan_cells_pending = false;
		m_scan.cells_status = -ECANCELED;
		hio_lte_flow_cmd("AT%NCELLMEASSTOP");
	}

	if (m_scan_plmn_pending) {
		m_scan.plmn_status = -ECANCELED;
	}

	hio_lte_flow_scan_end();
}

/* Shut the modem down without AT, for when the AT channel is stuck. */
int hio_lte_flow_abort(void)
{
	if (!nrf_modem_is_initialized()) {
		return 0;
	}

	return nrf_modem_lib_shutdown();
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

int hio_lte_flow_set_psk(const char *identity, const char *psk_hex)
{
	int ret;

	ret = hio_lte_talk_at_cfun(4);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cfun: 4` failed: %d", ret);
		return ret;
	}

	/* Delete */
	hio_lte_talk_at_cmng(3, SEC_TAG, 3, NULL);
	hio_lte_talk_at_cmng(3, SEC_TAG, 4, NULL);

	/* Set pre-shared key */
	ret = hio_lte_talk_at_cmng(0, SEC_TAG, 3, psk_hex);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmng` psk failed: %d", ret);
		return ret;
	}

	/* Set identity */
	ret = hio_lte_talk_at_cmng(0, SEC_TAG, 4, identity);
	if (ret) {
		LOG_ERR("Call `hio_lte_talk_at_cmng` identity failed: %d", ret);
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
		// delay is determined by the NEXT attempt: for next=odd => 168h, otherwise
		// 5m
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
