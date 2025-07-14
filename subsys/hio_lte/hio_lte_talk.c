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

LOG_MODULE_REGISTER(hio_lte_talk, CONFIG_HIO_LTE_LOG_LEVEL);

static char m_talk_buffer[512];

static int gather_prefix_values(const char *prefix, char *out_buf, size_t out_buf_size,
				int max_lines)
{
	size_t prefix_len = strlen(prefix);
	size_t used = 0;
	int found = 0;

	out_buf[0] = '\0';

	char *line = strtok(m_talk_buffer, "\r\n");
	while (line != NULL) {
		if (!strncmp(line, prefix, prefix_len)) {
			char *value = line + prefix_len;
			size_t val_len = strlen(value);

			if (used + val_len + 1 > out_buf_size) {
				return -ENOSPC;
			}

			memcpy(out_buf + used, value, val_len);
			used += val_len;
			out_buf[used++] = '\0';

			found++;
			if (max_lines > 0 && found >= max_lines) {
				break;
			}
		}
		line = strtok(NULL, "\r\n");
	}

	return found;
}

/* Split to tx and rx function is for nice logging */
static void rx(void)
{
	const char *ptr = m_talk_buffer;
	const char *line_start = ptr;

	while (*ptr != '\0') {
		if (*ptr == '\r' || *ptr == '\n') {
			if (line_start < ptr) {
				LOG_INF("%.*s", (int)(ptr - line_start), line_start);
			}
			while (*ptr == '\r' || *ptr == '\n') {
				ptr++;
			}
			line_start = ptr;
		} else {
			ptr++;
		}
	}
	if (line_start < ptr) {
		LOG_INF("%.*s", (int)(ptr - line_start), line_start);
	}
}

static int tx(const char *fmt, va_list args)
{
	int ret;
	static char cmd_buf[256];

	vsnprintf(cmd_buf, sizeof(cmd_buf), fmt, args);
	LOG_INF("%s", cmd_buf);
	ret = nrf_modem_at_cmd(m_talk_buffer, sizeof(m_talk_buffer), "%s", cmd_buf);
	m_talk_buffer[sizeof(m_talk_buffer) - 1] = '\0';

	if (ret < 0) {
		LOG_ERR("Call `nrf_modem_at_cmd` failed: %d", ret);
		return ret;
	} else if (ret > 0) {
		return -EILSEQ;
	}

	return ret;
}

static int cmd(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	int ret = tx(fmt, args);
	va_end(args);

	if (!ret) {
		rx();
	}

	return ret;
}

int hio_lte_talk_at_cclk_q(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT+CCLK?");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("+CCLK: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_ceppi(int p1)
{
	int ret;

	ret = cmd("AT+CEPPI=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cereg(int p1)
{
	int ret;

	ret = cmd("AT+CEREG=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cfun(int p1)
{
	int ret;

	ret = cmd("AT+CFUN=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cgauth(int p1, int *p2, const char *p3, const char *p4)
{
	int ret;

	if (!p2 && !p3 && !p4) {
		ret = cmd("AT+CGAUTH=%d", p1);
	} else if (p2 && !p3 && !p4) {
		ret = cmd("AT+CGAUTH=%d,%d", p1, *p2);
	} else if (p2 && p3 && !p4) {
		ret = cmd("AT+CGAUTH=%d,%d,\"%s\"", p1, *p2, p3);
	} else if (p2 && p3 && p4) {
		ret = cmd("AT+CGAUTH=%d,%d,\"%s\",\"%s\"", p1, *p2, p3, p4);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cgdcont(int p1, const char *p2, const char *p3)
{
	int ret;

	if (!p2 && !p3) {
		ret = cmd("AT+CGDCONT=%d", p1);
	} else if (p2 && !p3) {
		ret = cmd("AT+CGDCONT=%d,\"%s\"", p1, p2);
	} else if (p2 && p3) {
		ret = cmd("AT+CGDCONT=%d,\"%s\",\"%s\"", p1, p2, p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cgdcont_q(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT+CGDCONT?");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("+CGDCONT: ", buf, size, 0);
}

int hio_lte_talk_at_cgerep(int p1)
{
	int ret;

	ret = cmd("AT+CGEREP=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cgsn(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT+CGSN=1");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("+CGSN: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_cimi(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT+CIMI");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_iccid(char *buf, size_t size)
{
	return hio_lte_talk_at_cmd_with_resp_prefix("AT%XICCID", buf, size, "%XICCID: ");
}

int hio_lte_talk_at_cmee(int p1)
{
	int ret;

	ret = cmd("AT+CMEE=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cnec(int p1)
{
	int ret;

	ret = cmd("AT+CNEC=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_coneval(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT%%CONEVAL");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("%CONEVAL: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_cops_q(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT+COPS?");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("+COPS: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_cops(int p1, int *p2, const char *p3)
{
	int ret;

	if (!p2 && !p3) {
		ret = cmd("AT+COPS=%d", p1);
	} else if (p2 && !p3) {
		ret = cmd("AT+COPS=%d,%d", p1, *p2);
	} else if (p2 && p3) {
		ret = cmd("AT+COPS=%d,%d,\"%s\"", p1, *p2, p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cpsms(int *p1, const char *p2, const char *p3)
{
	int ret;

	if (!p1 && !p2 && !p3) {
		ret = cmd("AT+CPSMS");
	} else if (p1 && !p2 && !p3) {
		ret = cmd("AT+CPSMS=%d", *p1);
	} else if (p1 && p2 && !p3) {
		ret = cmd("AT+CPSMS=%d,\"\",\"\",\"%s\"", *p1, p2);
	} else if (p1 && p2 && p3) {
		ret = cmd("AT+CPSMS=%d,\"\",\"\",\"%s\",\"%s\"", *p1, p2, p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cscon(int p1)
{
	int ret;

	ret = cmd("AT+CSCON=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_hwversion(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT%%HWVERSION");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("%HWVERSION: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_mdmev(int p1)
{
	int ret;

	ret = cmd("AT%%MDMEV=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_rai(int p1)
{
	int ret;

	ret = cmd("AT%%RAI=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_rel14feat(int p1, int p2, int p3, int p4, int p5)
{
	int ret;

	ret = cmd("AT%%REL14FEAT=%d,%d,%d,%d,%d", p1, p2, p3, p4, p5);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_shortswver(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT%%SHORTSWVER");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("%SHORTSWVER: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_xbandlock(int p1, const char *p2)
{
	int ret;

	if (!p2) {
		ret = cmd("AT%%XBANDLOCK=%d", p1);
	} else {
		ret = cmd("AT%%XBANDLOCK=%d,\"%s\"", p1, p2);
	}
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xdataprfl(int p1)
{
	int ret;

	ret = cmd("AT%%XDATAPRFL=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xmodemsleep(int p1, int *p2, int *p3)
{
	int ret;

	if (!p2 && !p3) {
		ret = cmd("AT%%XMODEMSLEEP=%d", p1);
	} else if (p2 && p3) {
		ret = cmd("AT%%XMODEMSLEEP=%d,%d,%d", p1, *p2, *p3);
	} else {
		return -EINVAL;
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xnettime(int p1, int *p2)
{
	int ret;

	if (!p2) {
		ret = cmd("AT%%XNETTIME=%d", p1);
	} else {
		ret = cmd("AT%%XNETTIME=%d,%d", p1, *p2);
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xpofwarn(int p1, int p2)
{
	int ret;

	ret = cmd("AT%%XPOFWARN=%d,%d", p1, p2);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xsim(int p1)
{
	int ret;

	ret = cmd("AT%%XSIM=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xsocket(int p1, int *p2, int *p3, char *buf, size_t size)
{
	int ret;

	if (!p2 && !p3) {
		ret = cmd("AT#XSOCKET=%d", p1);
	} else if (p2 && p3) {
		ret = cmd("AT#XSOCKET=%d,%d,%d", p1, *p2, *p3);
	} else {
		return -EINVAL;
	}

	return gather_prefix_values("#XSOCKET: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_xsocketopt(int p1, int p2, int *p3)
{
	int ret;

	if (!p3) {
		ret = cmd("AT%%XSOCKETOPT=%d,%d", p1, p2);
	} else {
		ret = cmd("AT%%XSOCKETOPT=%d,%d,%d\"", p1, p2, *p3);
	}

	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xsystemmode(int p1, int p2, int p3, int p4)
{
	int ret;

	ret = cmd("AT%%XSYSTEMMODE=%d,%d,%d,%d", p1, p2, p3, p4);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xtemp(int p1)
{
	int ret;

	ret = cmd("AT%%XTEMP=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xtemphighlvl(int p1)
{
	int ret;

	ret = cmd("AT%%XTEMPHIGHLVL=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xtime(int p1)
{
	int ret;

	ret = cmd("AT%%XTIME=%d", p1);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_xversion(char *buf, size_t size)
{
	int ret;

	ret = cmd("AT#XVERSION");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("#XVERSION: ", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_xmodemtrace(int lvl)
{
	int ret;

	if (lvl == 0) {
		ret = cmd("AT%%XMODEMTRACE=0");
	} else if (lvl >= 1 && lvl <= 5) {
		ret = cmd("AT%%XMODEMTRACE=1,%d", lvl);
	} else {
		LOG_ERR("Invalid trace level: %d", lvl);
		return -EINVAL;
	}
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at()
{
	int ret;

	ret = cmd("AT");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_crsm_176(char *buf, size_t size)
{
	int ret;

	if (size < 24 + 2 + 1) {
		return -ENOBUFS;
	}

	ret = cmd("AT+CRSM=176,28539,0,0,12");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("+CRSM: 144,0,", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_crsm_214()
{
	int ret;

	char buf[4] = {0};

	ret = cmd("AT+CRSM=214,28539,0,0,12\"FFFFFFFFFFFFFFFFFFFFFFFF\"");
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	if (gather_prefix_values("+CRSM: 144,0,", buf, sizeof(buf), 1) < 0) {
		return -EILSEQ;
	}

	if (strcmp(buf, "\"\"")) {
		return -EILSEQ;
	}

	return 0;
}

int hio_lte_talk_at_cmd(const char *s)
{
	int ret;

	ret = cmd("%s", s);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return 0;
}

int hio_lte_talk_at_cmd_with_resp(const char *s, char *buf, size_t size)
{
	int ret;

	ret = cmd("%s", s);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values("", buf, size, 1) == 1 ? 0 : -EILSEQ;
}

int hio_lte_talk_at_cmd_with_resp_prefix(const char *s, char *buf, size_t size, const char *pfx)
{
	int ret;

	ret = cmd("%s", s);
	if (ret < 0) {
		LOG_ERR("Call `cmd` failed: %d", ret);
		return ret;
	}

	return gather_prefix_values(pfx, buf, size, 1) == 1 ? 0 : -EILSEQ;
}
