/**
 * \file    test_trust.c
 * \author  Kamil Kielbasa
 * \brief   Rollback counters, the wear history and the trust verdict.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/* Enough one-write appends to cross the interval have to fit in one block. */
BUILD_ASSERT(CONFIG_UBI_STATE_CHECK_INTERVAL *UBI_TEST_WRITE_BLOCK <
	     UBI_TEST_PEB_SIZE - UBI_DATA_OFFSET);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_trust);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a partition that has just been formatted.
 * When:  it is attached and asked what it counts.
 * Then:  the counters the application anchors rollback detection on describe
 *        exactly what a format writes: each volume table copy stamped once
 *        and sealed once, at the first revision.
 */
ZTEST(ubi_trust, test_the_rollback_counters_start_where_a_format_leaves_them)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.healthy_pebs);
	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.total_erase_count);
	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.global_sqnum);
	zassert_equal(1, info.revision);
}

/*
 * Given: a freshly formatted device.
 * When:  it is attached, which consults the application once.
 * Then:  what the callback was shown is what get_info reports, so a verdict
 *        can be reached on the same figures the application sees.
 */
ZTEST(ubi_trust, test_the_state_check_sees_what_get_info_reports)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(1, state_check_count, "an attach asks exactly once");
	zassert_equal(info.image_seq, last_state.image_seq);
	zassert_equal(info.global_sqnum, last_state.global_sqnum);
	zassert_equal(info.total_erase_count, last_state.total_erase_count);
	zassert_equal(info.healthy_pebs, last_state.healthy_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a formatted device and an application that trusts nothing.
 * When:  an attach is attempted.
 * Then:  it is refused read-only, so refusing to trust the flash is never a
 *        reason to change it.
 */
ZTEST(ubi_trust, test_an_untrusted_state_stops_the_attach)
{
	struct ubi_config guarded = config;

	guarded.state_cb = trust_nothing;

	zassert_ok(ubi_device_format(&config));

	const uint32_t before = partition_fingerprint();

	zassert_equal(-EROFS, ubi_device_init(ubi, &guarded));
	zassert_equal(1, state_check_count);
	zassert_equal(before, partition_fingerprint());

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an application that trusts the attach and nothing after it.
 * When:  appends carry on until the library consults it again.
 * Then:  the refusal stops the writes and latches, so the library refuses
 *        from then on without asking again and without writing.
 */
ZTEST(ubi_trust, test_a_withdrawn_trust_stops_the_writes)
{
	struct ubi_config watched = config;
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t record[UBI_TEST_WRITE_BLOCK] = { 0 };
	uint32_t appended = 0;
	int ret = 0;

	pattern_fill(record, sizeof(record), 0x3C);
	watched.state_cb = trust_once;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &watched));
	zassert_equal(1, state_check_count, "the attach has to ask once");
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	/* Each append is at least one write, so this many cross the
	 * interval. */
	while (0 == ret && appended < CONFIG_UBI_STATE_CHECK_INTERVAL) {
		ret = ubi_leb_write_at(ubi, vol_id, 0,
				       appended * sizeof(record), record,
				       sizeof(record));
		appended += 1;
	}

	zassert_equal(-EROFS, ret, "the interval has to bring the check back");
	zassert_equal(2, state_check_count);

	const uint32_t before = partition_fingerprint();

	zassert_equal(-EROFS, ubi_leb_write_at(ubi, vol_id, 0,
					       appended * sizeof(record),
					       record, sizeof(record)));
	zassert_equal(2, state_check_count, "a latched refusal does not ask");
	zassert_equal(before, partition_fingerprint());

	zassert_ok(ubi_device_deinit(ubi));
}
