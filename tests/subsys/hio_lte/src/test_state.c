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

/* History holds CONFIG_HIO_LTE_CEREG_HISTORY (3 here) entries. */
static void add_cereg(int stat, const char *tac, int cid, int cause_type, int cause)
{
	struct hio_lte_cereg_param param = {
		.valid = true,
		.stat = stat,
		.cid = cid,
		.act = HIO_LTE_CEREG_PARAM_ACT_LTE,
		.cause_type = cause_type,
		.reject_cause = cause,
	};

	strcpy(param.tac, tac);
	hio_lte_state_add_cereg_event(&param);
}

ZTEST(state_cereg_history, test_history)
{
	struct hio_lte_cereg_event e[5];
	size_t count;

	zassert_equal(hio_lte_state_get_last_reject(&e[0]), -ENODATA);

	add_cereg(2, "8DCC", 0xAE5CA, 0, 0);
	/* Repeat is not stored. */
	add_cereg(2, "8DCC", 0xAE5CA, 0, 0);
	add_cereg(2, "8DCC", 0xAE5CA, 0, 15);

	zassert_ok(hio_lte_state_get_cereg_history(e, ARRAY_SIZE(e), &count));
	zassert_equal(count, 2);
	zassert_equal(e[0].reject_cause, 15, "newest first");
	zassert_equal(e[0].tac, 0x8DCC);
	zassert_equal(e[0].cid, 0xAE5CA);

	/* Manufacturer-specific cause is not an EMM cause. */
	add_cereg(3, "8DCC", 0xAE5CA, 1, 42);
	add_cereg(5, "05F2", 0x74FEB50, 0, 0);
	add_cereg(1, "05F2", 0x74FEB50, 0, 0);

	zassert_ok(hio_lte_state_get_cereg_history(e, ARRAY_SIZE(e), &count));
	zassert_equal(count, 3, "ring keeps the newest 3");
	zassert_equal(e[0].stat, 1);
	zassert_equal(e[2].stat, 3);
	zassert_equal(e[2].reject_cause, 0);

	/* The reject survives the ring rolling over. */
	zassert_ok(hio_lte_state_get_last_reject(&e[0]));
	zassert_equal(e[0].reject_cause, 15);
	zassert_equal(e[0].tac, 0x8DCC);
}

ZTEST_SUITE(state_cereg_history, NULL, NULL, NULL, NULL, NULL);
