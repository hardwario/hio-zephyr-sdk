#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hio_lte_util.h>

#include <errno.h>
#include <stdio.h>

ZTEST(util_recv_timeout, test_default_without_cereg)
{
	zassert_equal(hio_lte_util_recv_timeout_sec(NULL), HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC);

	struct hio_lte_cereg_param cereg = {.valid = false};
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg),
		      HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC);
}

ZTEST(util_recv_timeout, test_granted_active_time_extends_window)
{
	/* Network granted T3324 of 10 s (the China Unicom NB-IoT case): the
	 * device stays reachable for paging that long after RRC release, so
	 * the receive window covers twice that to absorb RRC hold and server
	 * processing on top of the paging window. */
	struct hio_lte_cereg_param cereg = {
		.valid = true,
		.act = HIO_LTE_CEREG_PARAM_ACT_NBIOT,
		.active_time = 10,
	};
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg),
		      10 * HIO_LTE_UTIL_RECV_TIMEOUT_T3324_MULT);
}

ZTEST(util_recv_timeout, test_small_active_time_floors_to_default)
{
	struct hio_lte_cereg_param cereg = {
		.valid = true,
		.act = HIO_LTE_CEREG_PARAM_ACT_NBIOT,
		.active_time = 2,
	};
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg),
		      HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC);
}

ZTEST(util_recv_timeout, test_granted_active_time_is_capped)
{
	struct hio_lte_cereg_param cereg = {
		.valid = true,
		.act = HIO_LTE_CEREG_PARAM_ACT_NBIOT,
		.active_time = 180,
	};
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg), HIO_LTE_UTIL_RECV_TIMEOUT_MAX_SEC);
}

ZTEST(util_recv_timeout, test_nbiot_without_psm_uses_nbiot_default)
{
	struct hio_lte_cereg_param cereg = {
		.valid = true,
		.act = HIO_LTE_CEREG_PARAM_ACT_NBIOT,
		.active_time = -1,
	};
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg), HIO_LTE_UTIL_RECV_TIMEOUT_NBIOT_SEC);

	/* T3324 of 0 s means immediate PSM, there is no usable paging
	 * window, fall back to the NB-IoT default as well. */
	cereg.active_time = 0;
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg), HIO_LTE_UTIL_RECV_TIMEOUT_NBIOT_SEC);
}

ZTEST(util_recv_timeout, test_ltem_without_psm_keeps_default)
{
	struct hio_lte_cereg_param cereg = {
		.valid = true,
		.act = HIO_LTE_CEREG_PARAM_ACT_LTE,
		.active_time = -1,
	};
	zassert_equal(hio_lte_util_recv_timeout_sec(&cereg),
		      HIO_LTE_UTIL_RECV_TIMEOUT_DEFAULT_SEC);
}

ZTEST_SUITE(util_recv_timeout, NULL, NULL, NULL, NULL, NULL);

ZTEST(util_send_timeout, test_no_deadline_uses_max)
{
	/* K_FOREVER (init transfers) reaches us as "no deadline" -> the hard
	 * ceiling, so a stuck bearer setup is still eventually abandoned. */
	zassert_equal(hio_lte_util_send_timeout_sec(-1), HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC);
}

ZTEST(util_send_timeout, test_generous_deadline_capped_to_max)
{
	/* Plenty of time left, but one send must not block longer than the
	 * ceiling regardless. */
	zassert_equal(hio_lte_util_send_timeout_sec(600), HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC);
}

ZTEST(util_send_timeout, test_deadline_within_range_passed_through)
{
	/* Caller has 40 s left -> the send may wait up to 40 s, never past the
	 * caller's own deadline. */
	zassert_equal(hio_lte_util_send_timeout_sec(40), 40);
}

ZTEST(util_send_timeout, test_almost_expired_deadline_floors_to_min)
{
	/* Deadline nearly up: still give the send a usable minimum window
	 * rather than a doomed sub-second attempt. */
	zassert_equal(hio_lte_util_send_timeout_sec(1), HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC);
	zassert_equal(hio_lte_util_send_timeout_sec(0), HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC);
}

ZTEST(util_send_timeout, test_boundaries)
{
	zassert_equal(hio_lte_util_send_timeout_sec(HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC),
		      HIO_LTE_UTIL_SEND_TIMEOUT_MAX_SEC);
	zassert_equal(hio_lte_util_send_timeout_sec(HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC),
		      HIO_LTE_UTIL_SEND_TIMEOUT_MIN_SEC);
}

ZTEST_SUITE(util_send_timeout, NULL, NULL, NULL, NULL, NULL);

/* A scan before its first network search. */
static void scan_begun(struct hio_lte_scan_result *scan)
{
	*scan = (struct hio_lte_scan_result){.plmn_status = -ENODATA};
}

static void assert_entry(const struct hio_lte_scan_result *scan, size_t i, const char *plmn,
			 uint8_t act, uint8_t stat)
{
	zassert_str_equal(scan->entries[i].plmn, plmn, "entry %zu", i);
	zassert_equal(scan->entries[i].act, act, "entry %zu", i);
	zassert_equal(scan->entries[i].stat, stat, "entry %zu", i);
}

ZTEST(util_scan_merge, test_first_search)
{
	struct hio_lte_scan_result scan;
	const struct hio_lte_scan_entry found[] = {
		{.stat = 2, .act = 7, .plmn = "23003"},
		{.stat = 1, .act = 7, .plmn = "23001"},
	};

	scan_begun(&scan);
	hio_lte_util_scan_merge_networks(&scan, found, 2, 7, 0);

	zassert_equal(scan.count, 2);
	zassert_ok(scan.plmn_status);
	assert_entry(&scan, 0, "23003", 7, 2);
	assert_entry(&scan, 1, "23001", 7, 1);
}

/* The search of the other RAT adds its networks; a network of the first RAT
 * it lists again keeps the stat of the search in its own RAT. */
ZTEST(util_scan_merge, test_second_rat_adds)
{
	struct hio_lte_scan_result scan;
	const struct hio_lte_scan_entry ltem[] = {
		{.stat = 2, .act = 7, .plmn = "23003"},
		{.stat = 1, .act = 7, .plmn = "23001"},
	};
	const struct hio_lte_scan_entry nbiot[] = {
		{.stat = 2, .act = 9, .plmn = "23003"},
		{.stat = 1, .act = 7, .plmn = "23003"},
	};

	scan_begun(&scan);
	hio_lte_util_scan_merge_networks(&scan, ltem, 2, 7, 0);
	hio_lte_util_scan_merge_networks(&scan, nbiot, 2, 9, 0);

	zassert_equal(scan.count, 3);
	assert_entry(&scan, 0, "23003", 7, 2);
	assert_entry(&scan, 1, "23001", 7, 1);
	assert_entry(&scan, 2, "23003", 9, 2);
}

/* The search in a RAT gives the stat of its networks. */
ZTEST(util_scan_merge, test_own_rat_replaces)
{
	struct hio_lte_scan_result scan;
	const struct hio_lte_scan_entry nbiot[] = {
		{.stat = 1, .act = 7, .plmn = "23003"},
		{.stat = 2, .act = 9, .plmn = "23003"},
	};
	const struct hio_lte_scan_entry ltem[] = {
		{.stat = 2, .act = 7, .plmn = "23003"},
	};

	scan_begun(&scan);
	hio_lte_util_scan_merge_networks(&scan, nbiot, 2, 9, 0);
	hio_lte_util_scan_merge_networks(&scan, ltem, 1, 7, 0);

	zassert_equal(scan.count, 2);
	assert_entry(&scan, 0, "23003", 7, 2);
	assert_entry(&scan, 1, "23003", 9, 2);
}

/* CONFIG_HIO_LTE_SCAN_MAX_ENTRIES (10 here): more are counted, not stored;
 * the ones the search did not store count as well. */
ZTEST(util_scan_merge, test_full)
{
	struct hio_lte_scan_result scan;
	struct hio_lte_scan_entry found[CONFIG_HIO_LTE_SCAN_MAX_ENTRIES];

	for (int i = 0; i < ARRAY_SIZE(found); i++) {
		found[i] = (struct hio_lte_scan_entry){.stat = 1, .act = 7};
		snprintf(found[i].plmn, sizeof(found[i].plmn), "230%02d", i);
	}

	scan_begun(&scan);
	hio_lte_util_scan_merge_networks(&scan, found, 9, 7, 0);
	zassert_equal(scan.count, 9);

	/* 13 found, the first 10 stored: 3 of them new (one fits), 7 known. */
	found[0].act = 9;
	found[1].act = 9;
	hio_lte_util_scan_merge_networks(&scan, found, 13, 9, 0);

	zassert_equal(scan.count, 9 + 3 + 3);
	assert_entry(&scan, 9, "23000", 9, 1);
}

/* The status of the networks is the first error. */
ZTEST(util_scan_merge, test_status)
{
	struct hio_lte_scan_result scan;

	scan_begun(&scan);
	hio_lte_util_scan_merge_networks(&scan, NULL, 0, 7, 0);
	zassert_ok(scan.plmn_status);
	hio_lte_util_scan_merge_networks(&scan, NULL, 0, 9, -EINTR);
	zassert_equal(scan.plmn_status, -EINTR);

	scan_begun(&scan);
	hio_lte_util_scan_merge_networks(&scan, NULL, 0, 7, -EBUSY);
	hio_lte_util_scan_merge_networks(&scan, NULL, 0, 9, 0);
	zassert_equal(scan.plmn_status, -EBUSY);
	hio_lte_util_scan_merge_networks(&scan, NULL, 0, 9, -EIO);
	zassert_equal(scan.plmn_status, -EBUSY);
}

ZTEST_SUITE(util_scan_merge, NULL, NULL, NULL, NULL, NULL);

/* Cut short in a search: cancelled, unless a search before it failed. */
ZTEST(util_scan_cut, test_running)
{
	zassert_equal(hio_lte_util_scan_cut_status(-ENODATA, true, true), -ECANCELED);
	zassert_equal(hio_lte_util_scan_cut_status(0, true, false), -ECANCELED);
	zassert_equal(hio_lte_util_scan_cut_status(-EBUSY, true, true), -EBUSY);
	zassert_equal(hio_lte_util_scan_cut_status(-EIO, true, false), -EIO);
}

/* Cut short between the searches: partial when one ran, not run when none. */
ZTEST(util_scan_cut, test_missed)
{
	zassert_equal(hio_lte_util_scan_cut_status(0, false, true), -EINTR);
	zassert_equal(hio_lte_util_scan_cut_status(-ENODATA, false, true), -ENODATA);
	zassert_equal(hio_lte_util_scan_cut_status(-EIO, false, true), -EIO);
}

/* After the last search: as it is. */
ZTEST(util_scan_cut, test_none_left)
{
	zassert_ok(hio_lte_util_scan_cut_status(0, false, false));
	zassert_equal(hio_lte_util_scan_cut_status(-ENODATA, false, false), -ENODATA);
	zassert_equal(hio_lte_util_scan_cut_status(-EINTR, false, false), -EINTR);
}

ZTEST_SUITE(util_scan_cut, NULL, NULL, NULL, NULL, NULL);
