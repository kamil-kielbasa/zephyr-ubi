/**
 * \file    test_maintenance.c
 * \author  Kamil Kielbasa
 * \brief   Reclaiming, repairing the volume table and discarding, on the
 *          application's clock.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "partition.h"
#include "suite.h"
#include "table_copies.h"

/* Module defines ---------------------------------------------------------- */

/** Blocks to erase in one go, small enough to leave work behind. */
#define RECLAIM_BUDGET (4)

/** The first value past the last operation types.h defines. */
#define OPERATION_UNKNOWN \
	((enum ubi_maintenance_op)(UBI_MAINTENANCE_DISCARD + 1))

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_maintenance);

/* Module interface function definitions ----------------------------------- */

/* Tests: reclaiming ------------------------------------------------------- */

/*
 * Given: a freshly formatted device with blocks waiting to be reclaimed.
 * When:  maintenance is asked for with a budget of zero.
 * Then:  it only counts the work, touching nothing, and what it counts
 *        agrees with what get_info reports.
 */
ZTEST(ubi_maintenance, test_a_budget_of_zero_only_counts_the_work)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(0 < info.reclaimable_pebs, "there has to be work");

	const uint32_t before = partition_fingerprint();

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 0, &result));

	zassert_equal(0, result.performed, "zero means look, do not touch");
	zassert_equal(info.reclaimable_pebs, result.remaining,
		      "and what it reports has to be what get_info says");
	zassert_equal(before, partition_fingerprint(),
		      "nothing may have been erased");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a freshly formatted device with blocks waiting to be reclaimed.
 * When:  a budget of four is spent on reclaim.
 * Then:  exactly four blocks move to the free pool, allocatable without
 *        another erase, and the outstanding count drops by four.
 */
ZTEST(ubi_maintenance, test_reclaiming_refills_the_free_pool)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, RECLAIM_BUDGET,
				   &result));

	zassert_equal(RECLAIM_BUDGET, result.performed);
	zassert_equal(before.reclaimable_pebs - RECLAIM_BUDGET,
		      result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.free_pebs + RECLAIM_BUDGET, after.free_pebs,
		      "an erased block is allocatable without another erase");
	zassert_equal(before.reclaimable_pebs - RECLAIM_BUDGET,
		      after.reclaimable_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a released block that still carries data, and a blank block below
 *        it that is waiting for an erase as well.
 * When:  a single reclaim step runs.
 * Then:  it goes for the released block, so the data stops being readable
 *        at the first opportunity.
 */
ZTEST(ubi_maintenance, test_reclaiming_takes_released_blocks_first)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const struct flash_area *flash_area = NULL;
	struct ubi_maintenance_result result = { 0 };
	uint8_t blanked[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t released[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(blanked, sizeof(blanked), 0x70);
	pattern_fill(released, sizeof(released), 0x71);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, blanked, sizeof(blanked)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, released, sizeof(released)));
	zassert_ok(ubi_device_deinit(ubi));

	const uint32_t blank = pnum_of_data_matching(blanked, sizeof(blanked));

	zassert_true(blank < pnum_of_data_matching(released, sizeof(released)),
		     "the blank block has to come first in the partition");

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_erase(flash_area, blank * UBI_TEST_PEB_SIZE,
				    UBI_TEST_PEB_SIZE));
	flash_area_close(flash_area);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 1));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));

	zassert_equal(1, result.performed);
	zassert_equal(0, count_data_matching(released, sizeof(released)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device with a known amount of reclaimable work.
 * When:  a budget larger than the work is offered, twice.
 * Then:  it does what there is and stops, and asking again is not an error.
 */
ZTEST(ubi_maintenance, test_reclaiming_stops_when_there_is_nothing_left)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.reclaimable_pebs + 1, &result));

	zassert_equal(info.reclaimable_pebs, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(0, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: repairing the volume table --------------------------------------- */

/*
 * Given: a device attached with one volume table copy damaged.
 * When:  a repair is counted and then carried out.
 * Then:  the pair agrees again, and the repair reached the flash rather than
 *        only the bookkeeping.
 */
ZTEST(ubi_maintenance, test_repairing_clears_a_degraded_volume_table)
{
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_equal(1, corrupt_volume_tables(config.ikm_key_id, 1));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 0, &result));
	zassert_equal(1, result.remaining, "the pair does not agree yet");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(0, events_total, "the repair has to reach the flash");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose volume table copies already agree.
 * When:  a repair is asked for.
 * Then:  nothing is done and nothing is written, because a healthy pair must
 *        not be rewritten.
 */
ZTEST(ubi_maintenance, test_repairing_a_healthy_table_does_nothing)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));

	zassert_equal(0, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.max_sqnum, after.max_sqnum,
		      "a healthy pair must not be rewritten");
	zassert_equal(before.revision, after.revision);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: the argument contract -------------------------------------------- */

/*
 * Given: an attached device.
 * When:  maintenance is asked for without a result, or with an operation
 *        that does not exist.
 * Then:  each is refused as a bad argument.
 */
ZTEST(ubi_maintenance, test_an_unknown_maintenance_operation_is_refused)
{
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EINVAL,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, NULL),
		      "a caller has to take the result it asked for");

	zassert_equal(-EINVAL,
		      ubi_maintenance(ubi, OPERATION_UNKNOWN, 1, &result));
	zassert_equal(-EINVAL,
		      ubi_maintenance(ubi, OPERATION_UNKNOWN, 0, &result));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: discarding ------------------------------------------------------- */

/*
 * Given: a block whose volume identifier header was damaged while its data
 *        survived, which the attach keeps for whoever may still want what it
 *        holds.
 * When:  the application counts, and then discards it.
 * Then:  it is erased and returns to the free pool, and the next attach no
 *        longer reports it.
 */
ZTEST(ubi_maintenance, test_discarding_frees_a_block_kept_for_its_data)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t kept[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(kept, sizeof(kept), 0xD1);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, kept, sizeof(kept)));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_equal(1, corrupt_header_of_data_matching(kept, sizeof(kept)));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_equal(1, before.corrupt_pebs);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_DISCARD, 0, &result));
	zassert_equal(0, result.performed);
	zassert_equal(1, result.remaining);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_DISCARD, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(0, after.corrupt_pebs);
	zassert_equal(before.free_pebs + 1, after.free_pebs);
	zassert_equal(0, count_data_matching(kept, sizeof(kept)),
		      "a discarded block is erased");

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(0, after.corrupt_pebs);
	zassert_ok(ubi_device_deinit(ubi));
}
