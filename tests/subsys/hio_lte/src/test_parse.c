/* west twister --testsuite-root . -c -i -v -p native_posix */
/* west build -b native_posix && ./build/hio_lte/zephyr/zephyr.elf */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hio_lte_parse.h>
static void expect_ok(const char *s, int exp_plmn, int16_t exp_mcc, int16_t exp_mnc)
{
	int plmn;
	int16_t mcc, mnc;
	int ret = hio_lte_parse_plmn(s, &plmn, &mcc, &mnc);
	zassert_ok(ret, "parse failed for '%s'", s);
	zassert_equal(plmn, exp_plmn, "plmn mismatch for '%s'", s);
	zassert_equal(mcc, exp_mcc, "mcc mismatch for '%s'", s);
	zassert_equal(mnc, exp_mnc, "mnc mismatch for '%s'", s);
}

ZTEST(parser, test_plmn_2digit_ok)
{
	expect_ok("23003", 23003, 230, 3);
	expect_ok("26295", 26295, 262, 95);
	expect_ok("20416", 20416, 204, 16);
}

ZTEST(parser, test_plmn_3digit_ok)
{
	expect_ok("310260", 310260, 310, 260);
}

ZTEST(parser, test_plmn_null_outputs_ok)
{
	int ret = hio_lte_parse_plmn("23003", NULL, NULL, NULL);
	zassert_ok(ret, "parse with NULL outputs failed");
}

ZTEST(parser, test_plmn_invalid_length)
{
	int plmn;
	int16_t mcc, mnc;
	int ret = hio_lte_parse_plmn("1234", &plmn, &mcc, &mnc);
	zassert_equal(ret, -EBADMSG, "expected -EBADMSG for invalid len");
}

ZTEST(parser, test_plmn_non_digit)
{
	int plmn;
	int16_t mcc, mnc;
	int ret = hio_lte_parse_plmn("3102a0", &plmn, &mcc, &mnc);
	zassert_equal(ret, -EBADMSG, "expected -EBADMSG for non-digit");
}

ZTEST(parser, test_plmn_mcc_000)
{
	int plmn;
	int16_t mcc, mnc;
	int ret = hio_lte_parse_plmn("00001", &plmn, &mcc, &mnc);
	zassert_equal(ret, -EPROTO, "expected -EPROTO for MCC=000");
}

ZTEST(parser, test_plmn_mnc_leading_zero)
{
	expect_ok("23007", 23007, 230, 7);
}

ZTEST(parser, test_urc_cereg_5)
{
	struct hio_lte_cereg_param param;
	int ret = hio_lte_parse_urc_cereg("5,\"AF66\",\"009DE067\",9,,,\"00000000\",\"00111000\"",
					  &param);

	zassert_ok(ret, "hio_lte_parse_urc_cereg failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.stat, 5, "param.stat not equal");
	zassert_true(strcmp(param.tac, "AF66") == 0, "param.tac not equal");
	zassert_equal(param.cid, 0x009DE067, "param.cid not equal");
	zassert_equal(param.act, 9, "param.act not equal");
	zassert_equal(param.cause_type, 0, "param.cause_type not equal");
	zassert_equal(param.reject_cause, 0, "param.reject_cause not equal");
	zassert_equal(param.active_time, 0, "param.active_time not equal");
	zassert_equal(param.periodic_tau_ext, 86400, "param.periodic_tau_ext not equal");
}

ZTEST(parser, test_urc_cereg_5_active_time)
{
	struct hio_lte_cereg_param param;
	int ret = hio_lte_parse_urc_cereg("5,\"AF66\",\"009DE067\",9,,,\"00000101\",\"00111000\"",
					  &param);

	zassert_ok(ret, "hio_lte_parse_urc_cereg failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.stat, 5, "param.stat not equal");
	zassert_true(strcmp(param.tac, "AF66") == 0, "param.tac not equal");
	zassert_equal(param.cid, 0x009DE067, "param.cid not equal");
	zassert_equal(param.act, 9, "param.act not equal");
	zassert_equal(param.cause_type, 0, "param.cause_type not equal");
	zassert_equal(param.reject_cause, 0, "param.reject_cause not equal");
	zassert_equal(param.active_time, 10, "param.active_time not equal");
	zassert_equal(param.periodic_tau_ext, 86400, "param.periodic_tau_ext not equal");
}

ZTEST(parser, test_urc_cereg_5_psm_disabled)
{
	struct hio_lte_cereg_param param;
	int ret = hio_lte_parse_urc_cereg("5,\"3866\",\"074FEB0C\",7,,,\"11100000\",\"11100000\"",
					  &param);

	zassert_ok(ret, "hio_lte_parse_urc_cereg failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.stat, 5, "param.stat not equal");
	zassert_true(strcmp(param.tac, "3866") == 0, "param.tac not equal");
	zassert_equal(param.cid, 0x074FEB0C, "param.cid not equal");
	zassert_equal(param.act, 7, "param.act not equal");
	zassert_equal(param.cause_type, 0, "param.cause_type not equal");
	zassert_equal(param.reject_cause, 0, "param.reject_cause not equal");
	zassert_equal(param.active_time, -1, "param.active_time not equal");
	zassert_equal(param.periodic_tau_ext, -1, "param.periodic_tau_ext not equal");
}

ZTEST(parser, test_urc_cereg_2)
{
	struct hio_lte_cereg_param param;
	int ret = hio_lte_parse_urc_cereg("2,\"B4DC\",\"000AE520\",9", &param);

	zassert_ok(ret, "hio_lte_parse_urc_cereg failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.stat, 2, "param.stat not equal");
	zassert_true(strcmp(param.tac, "B4DC") == 0, "param.tac not equal");
	zassert_equal(param.cid, 0x000AE520, "param.cid not equal");
	zassert_equal(param.act, 9, "param.act not equal");
}

ZTEST(parser, test_urc_cereg_2_empty)
{
	struct hio_lte_cereg_param param;
	int ret = hio_lte_parse_urc_cereg("2", &param);

	zassert_ok(ret, "hio_lte_parse_urc_cereg failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.stat, 2, "param.stat not equal");
	zassert_true(strcmp(param.tac, "") == 0, "param.tac not equal");
	zassert_equal(param.cid, 0, "param.cid not equal");
	zassert_equal(param.act, 0, "param.act not equal");
}

ZTEST(parser, test_urc_cereg_4)
{
	struct hio_lte_cereg_param param;
	int ret = hio_lte_parse_urc_cereg("4", &param);

	zassert_ok(ret, "hio_lte_parse_urc_cereg failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.stat, 4, "param.stat not equal");
	zassert_true(strcmp(param.tac, "") == 0, "param.tac not equal");
	zassert_equal(param.cid, 0, "param.cid not equal");
	zassert_equal(param.act, 0, "param.act not equal");
}

ZTEST(parser, test_urc_xmodemsleep_1_89999825)
{
	int p1, p2;
	int ret = hio_lte_parse_urc_xmodemsleep("1,89999825", &p1, &p2);

	zassert_ok(ret, "hio_lte_parse_urc_xmodemsleep failed");
	zassert_equal(p1, 1, "p1 not equal");
	zassert_equal(p2, 89999825, "p2 not equal");
}

ZTEST(parser, test_urc_xmodemsleep_1_0)
{
	int p1, p2;
	int ret = hio_lte_parse_urc_xmodemsleep("1,0", &p1, &p2);

	zassert_ok(ret, "hio_lte_parse_urc_xmodemsleep failed");
	zassert_equal(p1, 1, "p1 not equal");
	zassert_equal(p2, 0, "p2 not equal");
}

ZTEST(parser, test_urc_xmodemsleep_4_0)
{
	int p1, p2;
	int ret = hio_lte_parse_urc_xmodemsleep("4,0", &p1, &p2);

	zassert_ok(ret, "hio_lte_parse_urc_xmodemsleep failed");
	zassert_equal(p1, 4, "p1 not equal");
	zassert_equal(p2, 0, "p2 not equal");
}

ZTEST(parser, test_urc_xmodemsleep_4)
{
	int p1, p2;
	int ret = hio_lte_parse_urc_xmodemsleep("4", &p1, &p2);

	zassert_ok(ret, "hio_lte_parse_urc_xmodemsleep failed");
	zassert_equal(p1, 4, "p1 not equal");
	zassert_equal(p2, 0, "p2 not equal");
}

ZTEST(parser, test_rai)
{
	struct hio_lte_rai_param param;
	int ret = hio_lte_parse_urc_rai("\"000AE520\",\"23003\",1,0", &param);

	zassert_ok(ret, "hio_lte_parse_urc_rai failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.cell_id, 0x000AE520, "param.cell_id not equal");
	zassert_equal(param.plmn, 23003, "param.plmn not equal");
	zassert_equal(param.as_rai, 1, "param.as_rai not equal");
	zassert_equal(param.cp_rai, 0, "param.cp_rai not equal");
}

ZTEST(parser, test_coneval)
{

	struct hio_lte_conn_param param;
	int ret = hio_lte_parse_coneval(
		"0,1,7,68,29,47,\"000AE520\",\"23003\",135,6447,20,0,0,14,2,1,99", &param);

	zassert_ok(ret, "hio_lte_parse_coneval failed");
	zassert_equal(param.valid, true, "param.valid not true");
	zassert_equal(param.result, 0, "param.result not equal"); // success
	zassert_equal(param.eest, 7, "param.eest not equal");     // energy_estimate
	zassert_equal(param.ecl, 0, "param.ecl not equal");
	zassert_equal(param.rsrp, -72, "param.rsrp not equal");
	zassert_equal(param.rsrq, -5, "param.rsrq not equal");
	zassert_equal(param.snr, 23, "param.snr not equal");
	zassert_equal(param.plmn, 23003, "param.plmn not equal");
	zassert_equal(param.cid, 0x000AE520, "param.cid not equal");
	zassert_equal(param.band, 20, "param.band not equal");
	zassert_equal(param.earfcn, 6447, "param.earfcn not equal");
}

ZTEST(parser, test_cgcont)
{
	struct cgdcont_param param;
	int ret = hio_lte_parse_cgcont("0,\"IP\",\"iot.1nce.net\",\"10.52.2.149\",0,0", &param);

	zassert_ok(ret, "hio_lte_parse_cgcont failed");
	zassert_equal(param.cid, 0, "param.cid not equal");
	zassert_true(strcmp(param.pdn_type, "IP") == 0, "param.pdn_type not equal");
	zassert_true(strcmp(param.apn, "iot.1nce.net") == 0, "param.apn not equal");
	zassert_true(strcmp(param.addr, "10.52.2.149") == 0, "param.addr not equal");
}

ZTEST(parser, test_cgcont_no_address)
{
	struct cgdcont_param param;
	int ret = hio_lte_parse_cgcont("1,\"IPV6\",\"example.apn\",\"\",0,0", &param);

	zassert_ok(ret, "hio_lte_parse_cgcont failed");
	zassert_equal(param.cid, 1, "param.cid not equal");
	zassert_true(strcmp(param.pdn_type, "IPV6") == 0, "param.pdn_type not equal");
	zassert_true(strcmp(param.apn, "example.apn") == 0, "param.apn not equal");
	zassert_true(strcmp(param.addr, "") == 0, "param.addr not equal");
}

ZTEST_SUITE(parser, NULL, NULL, NULL, NULL, NULL);
