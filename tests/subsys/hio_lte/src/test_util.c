#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hio_lte_util.h>

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
