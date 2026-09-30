#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hio/hio_lte.h>

ZTEST(str_cause, test_emm_cause)
{
	zassert_str_equal(hio_lte_str_emm_cause(15), "No suitable cells in tracking area");
	zassert_not_null(hio_lte_str_emm_cause_hint(15));
	zassert_str_equal(hio_lte_str_emm_cause(111), "Protocol error, unspecified");
}

ZTEST(str_cause, test_esm_cause)
{
	zassert_str_equal(hio_lte_str_esm_cause(27), "Missing or unknown APN");
	zassert_not_null(hio_lte_str_esm_cause_hint(27));
}

ZTEST(str_cause, test_unknown_cause)
{
	zassert_str_equal(hio_lte_str_emm_cause(0), "unknown");
	zassert_str_equal(hio_lte_str_esm_cause(255), "unknown");
	zassert_is_null(hio_lte_str_emm_cause_hint(0));
	zassert_is_null(hio_lte_str_emm_cause_hint(16));
}

ZTEST_SUITE(str_cause, NULL, NULL, NULL, NULL, NULL);
