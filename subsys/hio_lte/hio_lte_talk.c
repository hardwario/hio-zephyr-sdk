#include "hio_lte_talk.h"

/* NRF includes */
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>
#include <modem/at_monitor.h>
#include <modem/modem_info.h>
#include <nrf_errno.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

LOG_MODULE_REGISTER(hio_lte_talk, LOG_LEVEL_INF);

static char m_talk_buffer[512];

#define MATCH_PREFIX(pfx)                                                                          \
	{                                                                                          \
		char *line = strtok(m_talk_buffer, "\r\n");                                        \
		while (line != NULL) {                                                             \
			if (!strncmp(line, pfx, strlen(pfx))) {                                    \
				return 0;                                                          \
			}                                                                          \
			line = strtok(NULL, "\r\n");                                               \
		}                                                                                  \
		return -EILSEQ;                                                                    \
	}

#define GATHER_PREFIX(pfx, buf, size)                                                              \
	{                                                                                          \
		char *line = strtok(m_talk_buffer, "\r\n");                                        \
		while (line != NULL) {                                                             \
			if (!strncmp(line, pfx, strlen(pfx)) && size) {                            \
				if (strlen(&line[strlen(pfx)]) >= size) {                          \
					return -ENOSPC;                                            \
				} else if (buf != NULL) {                                          \
					char *start = &line[strlen(pfx)];                          \
					strncpy(buf, start, size - 1);                             \
					buf[size - 1] = '\0';                                      \
					char *end = buf + strlen(buf);                             \
					if (end > buf && *(end - 1) == '\n')                       \
						*(end - 1) = '\0';                                 \
					if (end > buf && *(end - 1) == '\r')                       \
						*(end - 1) = '\0';                                 \
				}                                                                  \
				break;                                                             \
			}                                                                          \
			line = strtok(NULL, "\r\n");                                               \
		}                                                                                  \
	}

#define GATHER_PREFIX_RET(pfx, buf, size)                                                          \
	{                                                                                          \
		char *line = strtok(m_talk_buffer, "\r\n");                                        \
		while (line != NULL) {                                                             \
			if (!strncmp(line, pfx, strlen(pfx)) && size) {                            \
				if (strlen(&line[strlen(pfx)]) >= size) {                          \
					return -ENOSPC;                                            \
				} else if (buf != NULL) {                                          \
					char *start = &line[strlen(pfx)];                          \
					strncpy(buf, start, size - 1);                             \
					buf[size - 1] = '\0';                                      \
					char *end = buf + strlen(buf);                             \
					if (end > buf && *(end - 1) == '\n')                       \
						*(end - 1) = '\0';                                 \
					if (end > buf && *(end - 1) == '\r')                       \
						*(end - 1) = '\0';                                 \
				}                                                                  \
				return 0;                                                          \
			}                                                                          \
			line = strtok(NULL, "\r\n");                                               \
		}                                                                                  \
		return -EILSEQ;                                                                    \
	}

int hio_lte_talk_(const char *s)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "%s", s);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cclk_q(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CCLK?");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("+CCLK: ", buf, size);
}

int hio_lte_talk_at_ceppi(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CEPPI=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cereg(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CEREG=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cfun(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CFUN=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cgauth(int p1, int *p2, const char *p3, const char *p4)
{
	int ret;

	if (!p2 && !p3 && !p4) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CGAUTH=%d", p1);
	} else if (p2 && !p3 && !p4) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CGAUTH=%d,%d", p1,
				       *p2);
	} else if (p2 && p3 && !p4) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
				       "AT+CGAUTH=%d,%d,\"%s\"", p1, *p2, p3);
	} else if (p2 && p3 && p4) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
				       "AT+CGAUTH=%d,%d,\"%s\",\"%s\"", p1, *p2, p3, p4);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cgdcont(int p1, const char *p2, const char *p3)
{
	int ret;

	if (!p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CGDCONT=%d", p1);
	} else if (p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CGDCONT=%d,\"%s\"",
				       p1, p2);
	} else if (p2 && p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
				       "AT+CGDCONT=%d,\"%s\",\"%s\"", p1, p2, p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cgerep(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CGEREP=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cgsn(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CGSN=1");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("+CGSN: ", buf, size);
}

int hio_lte_talk_at_cimi(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CIMI");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("", buf, size);
}

int hio_lte_talk_at_iccid(char *buf, size_t size)
{
	return hio_lte_talk_at_cmd_with_resp_prefix("AT%XICCID", buf, size, "%XICCID: ");
}

int hio_lte_talk_at_cmee(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CMEE=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cnec(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CNEC=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_coneval(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%CONEVAL");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("%CONEVAL: ", buf, size);
}

int hio_lte_talk_at_cops_q(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+COPS?");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("+COPS: ", buf, size);
}

int hio_lte_talk_at_cops(int p1, int *p2, const char *p3)
{
	int ret;

	if (!p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+COPS=%d", p1);
	} else if (p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+COPS=%d,%d", p1,
				       *p2);
	} else if (p2 && p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+COPS=%d,%d,\"%s\"",
				       p1, *p2, p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cpsms(int *p1, const char *p2, const char *p3)
{
	int ret;

	if (!p1 && !p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CPSMS");
	} else if (p1 && !p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CPSMS=%d", *p1);
	} else if (p1 && p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CPSMS=%d,\"%s\"",
				       *p1, p2);
	} else if (p1 && p2 && p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
				       "AT+CPSMS=%d,\"%s\",\"%s\"", *p1, p2, p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cscon(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CSCON=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_hwversion(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%HWVERSION");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("%HWVERSION: ", buf, size);
}

int hio_lte_talk_at_mdmev(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%MDMEV=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_rai(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%RAI=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_rel14feat(int p1, int p2, int p3, int p4, int p5)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%REL14FEAT=%d,%d,%d,%d,%d",
			       p1, p2, p3, p4, p5);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_shortswver(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%SHORTSWVER");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("%SHORTSWVER: ", buf, size);
}

int hio_lte_talk_at_xbandlock(int p1, const char *p2)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XBANDLOCK=%d,\"%s\"", p1,
			       p2);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xdataprfl(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XDATAPRFL=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xmodemsleep(int p1, int *p2, int *p3)
{
	int ret;

	if (!p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XMODEMSLEEP=%d",
				       p1);
	} else if (p2 && p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
				       "AT%%XMODEMSLEEP=%d,%d,%d", p1, *p2, *p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xnettime(int p1, int *p2)
{
	int ret;

	if (!p2) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XNETTIME=%d", p1);
	} else {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XNETTIME=%d,%d",
				       p1, *p2);
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xpofwarn(int p1, int p2)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XPOFWARN=%d,%d", p1, p2);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xsim(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XSIM=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xsleep(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XSLEEP=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xsocket(int p1, int *p2, int *p3, char *buf, size_t size)
{
	int ret;

	if (!p2 && !p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT#XSOCKET=%d", p1);
	} else if (p2 && p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT#XSOCKET=%d,%d,%d",
				       p1, *p2, *p3);
	} else {
		return -EINVAL;
	}

	GATHER_PREFIX_RET("#XSOCKET: ", buf, size);
}

int hio_lte_talk_at_xsocketopt(int p1, int p2, int *p3)
{
	int ret;

	if (!p3) {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XSOCKETOPT=%d,%d",
				       p1, p2);
	} else {
		ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
				       "AT%%XSOCKETOPT=%d,%d,%d\"", p1, p2, *p3);
	}

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xsystemmode(int p1, int p2, int p3, int p4)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XSYSTEMMODE=%d,%d,%d,%d",
			       p1, p2, p3, p4);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xtemp(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XTEMP=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xtemphighlvl(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XTEMPHIGHLVL=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xtime(int p1)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XTIME=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_xversion(char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT#XVERSION");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("#XVERSION: ", buf, size);
}

int hio_lte_talk_at_xmodemtrace()
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT%%XMODEMTRACE=1,2");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at()
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_crsm_176(char *buf, size_t size)
{
	int ret;

	if (size < 24 + 2 + 1) {
		return -ENOBUFS;
	}

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "AT+CRSM=176,28539,0,0,12");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("+CRSM: 144,0,", buf, size);
}

int hio_lte_talk_crsm_214()
{
	int ret;

	char buf[4] = {0};

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer),
			       "AT+CRSM=214,28539,0,0,12\"FFFFFFFFFFFFFFFFFFFFFFFF\"");
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX("+CRSM: 144,0,", buf, sizeof(buf));

	if (strcmp(buf, "\"\"")) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cmd(const char *s)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "%s", s);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cmd_with_resp(const char *s, char *buf, size_t size)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "%s", s);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET("", buf, size);
}

int hio_lte_talk_at_cmd_with_resp_prefix(const char *s, char *buf, size_t size, const char *pfx)
{
	int ret;

	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "%s", s);
	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	GATHER_PREFIX_RET(pfx, buf, size);
}
