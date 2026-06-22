#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hio_lte_state.h>

ZTEST(state_ceer, test_ceer_round_trip)
{
	/* Unset state reports no data (must run before the set below — the
	 * state is a static singleton shared by all tests). */
	char *ceer = NULL;
	zassert_equal(hio_lte_state_get_ceer(&ceer), -ENODATA);

	hio_lte_state_set_ceer("RRC connection release, extended wait time 300 s");

	zassert_ok(hio_lte_state_get_ceer(&ceer));
	zassert_not_null(ceer);
	zassert_equal(strcmp(ceer, "RRC connection release, extended wait time 300 s"), 0,
		      "unexpected ceer: %s", ceer);
}

ZTEST(state_ceer, test_ceer_null_arg)
{
	zassert_equal(hio_lte_state_get_ceer(NULL), -EINVAL);
}

ZTEST_SUITE(state_ceer, NULL, NULL, NULL, NULL, NULL);
