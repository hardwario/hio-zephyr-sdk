#include "hio_lte_parse.h"

/* HIO includes */
#include <hio/hio_tok.h>

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

LOG_MODULE_REGISTER(hio_lte_parse, CONFIG_HIO_LTE_LOG_LEVEL);

static inline int all_digits_ascii(const char *s, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c < '0' || c > '9') {
			return 0;
		}
	}
	return 1;
}

static inline int all_hex_digits_ascii(const char *s, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if (!isxdigit(c)) {
			return 0;
		}
	}
	return 1;
}

static int parse_hex2cellid(char *s, int *cid)
{
	if (!s || !cid) {
		return -EINVAL;
	}

	size_t len = strlen(s);
	if (len != 8) {
		return -EBADMSG;
	}

	if (!all_hex_digits_ascii(s, len)) {
		return -EBADMSG;
	}

	char *endptr;
	unsigned long val = strtoul(s, &endptr, 16);
	if (*endptr != '\0' || val > 0xFFFFFFFFUL) {
		return -ERANGE;
	}

	*cid = (int)val;

	if (*cid < 0 || *cid > HIO_LTE_CELL_ECI_MAX) {
		return -ERANGE;
	}

	return 0;
}

static int parse_hex2tac(const char *s, uint16_t *tac)
{
	if (!s || !tac) {
		return -EINVAL;
	}

	size_t len = strlen(s);
	if (len != 4) {
		return -EBADMSG;
	}

	if (!all_hex_digits_ascii(s, len)) {
		return -EBADMSG;
	}

	char *endptr;
	unsigned long val = strtoul(s, &endptr, 16);
	if (*endptr != '\0' || val > 0xFFFFUL) {
		return -ERANGE;
	}

	*tac = (uint16_t)val;
	return 0;
}

static int parse_gprs_timer(const char *binary_string, int flag)
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

int hio_lte_parse_plmn(const char *str, int *plmn, int16_t *mcc, int16_t *mnc)
{
	if (!str) {
		return -EINVAL;
	}

	size_t len = strlen(str);
	if (len != 5 && len != 6) {
		return -EBADMSG;
	}

	if (!all_digits_ascii(str, len)) {
		return -EBADMSG;
	}

	int mcc_val = (str[0] - '0') * 100 + (str[1] - '0') * 10 + (str[2] - '0');
	if (mcc_val == 0) {
		return -EPROTO;
	}

	int mnc_val;
	if (len == 5) {
		mnc_val = (str[3] - '0') * 10 + (str[4] - '0'); /* 2-digit MNC */
	} else {
		mnc_val = (str[3] - '0') * 100 + (str[4] - '0') * 10 +
			  (str[5] - '0'); /* 3-digit MNC */
	}

	if (plmn) {
		int factor = (len == 5) ? 100 : 1000; /* 2-digit vs 3-digit MNC */
		*plmn = mcc_val * factor + mnc_val;
	}

	if (mcc) {
		*mcc = (int16_t)mcc_val;
	}

	if (mnc) {
		*mnc = (int16_t)mnc_val;
	}

	return 0;
}

int hio_lte_parse_urc_cereg(const char *line, struct hio_lte_cereg_param *param)
{
	/*
	<stat>[,[<tac>],[<ci>],[<AcT>][,<cause_type>],[<reject_cause>][,[<Active-Time>],[<Periodic-TAU-ext>]]]]
	 5,"AF66","009DE067",9,,,"00000000","00111000"
	 2,"B4DC","000AE520",9
	*/

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

	if (parse_hex2cellid(str, &param->cid) != 0) {
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

int hio_lte_parse_urc_xmodemsleep(const char *line, int *p1, int *p2)
{
	/* 1,86399999 */
	/* 4 */

	if (!line) {
		return -EINVAL;
	}

	const char *p = line;

	bool def;
	uint32_t num;

	*p1 = 0;
	*p2 = 0;

	if (!(p = hio_tok_uint32(p, &def, &num))) {
		return -EINVAL;
	}

	if (p1) {
		*p1 = num;
	}

	if (hio_tok_end(p)) {
		return 0;
	}

	if (!(p = hio_tok_sep(p))) {
		return -EINVAL;
	}

	if (!(p = hio_tok_uint32(p, &def, &num))) {
		return -EINVAL;
	}

	if (p2) {
		*p2 = num;
	}

	return 0;
}

int hio_lte_parse_urc_rai(const char *line, struct hio_lte_rai_param *param)
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

int hio_lte_parse_coneval(const char *str, struct hio_lte_conn_param *params)
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
	char cid[8 + 1] = {0};
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

	if (parse_hex2cellid(cid, &params->cid) != 0) {
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

int hio_lte_parse_cgcont(const char *line, struct cgdcont_param *param)
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
