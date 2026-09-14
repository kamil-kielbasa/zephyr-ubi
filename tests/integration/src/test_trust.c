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
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_integration, test_the_rollback_counters_survive_a_reattach)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	/* Only the two blocks holding the volume table have been stamped, and
	 * they carry the only sequence numbers issued so far. */
	zassert_equal(2, info.healthy_pebs);
	zassert_equal(2, info.total_erase_count);
	zassert_equal(2, info.global_sqnum);
	zassert_equal(1, info.revision);
}

ZTEST(ubi_integration, test_reformatting_does_not_restart_the_sequence_numbers)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	/*
	 * A format leaves most blocks untouched, so if it restarted the
	 * numbering a volume table left by the previous image would outrank
	 * the fresh one and the next attach would quietly undo the format.
	 */
	zassert_true(
		second.global_sqnum > first.global_sqnum,
		"a reformat must outrank whatever is already on the flash");
}

ZTEST(ubi_integration, test_reformatting_keeps_the_wear_history)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	/* A reformat reads the old erase count back and carries on from it,
	 * so the total never drops: that is what makes it usable as a
	 * rollback anchor. */
	zassert_true(second.total_erase_count > first.total_erase_count);
}

ZTEST(ubi_integration, test_the_state_check_sees_what_get_info_reports)
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

ZTEST(ubi_integration, test_an_untrusted_state_stops_the_attach)
{
	struct ubi_config guarded = config;

	guarded.state_cb = trust_nothing;

	zassert_ok(ubi_device_format(&config));

	const uint32_t before = partition_fingerprint();

	zassert_equal(-EROFS, ubi_device_init(ubi, &guarded));
	zassert_equal(1, state_check_count);

	/* Refusing to trust the flash must not be a reason to change it. */
	zassert_equal(before, partition_fingerprint());

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_withdrawn_trust_stops_the_writes)
{
	struct ubi_config watched = config;
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 2 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	int ret = 0;

	watched.state_cb = trust_once;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &watched));
	zassert_equal(1, state_check_count, "the attach has to ask once");

	/* Each round costs a handful of writes, so one of them is bound to
	 * cross the interval and ask again. */
	for (uint32_t round = 0; round < 64 && 0 == ret; ++round) {
		ret = ubi_volume_create(ubi, &wanted, &vol_id);

		if (0 == ret)
			ret = ubi_volume_remove(ubi, vol_id);
	}

	zassert_equal(-EROFS, ret, "a refusal has to stop the writes");
	zassert_true(state_check_count > 1);

	/* Latched: the library does not ask again, it simply refuses. */
	const uint32_t asked = state_check_count;

	zassert_equal(-EROFS, ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_equal(asked, state_check_count);

	zassert_ok(ubi_device_deinit(ubi));
}
