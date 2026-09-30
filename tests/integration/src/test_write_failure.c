/**
 * \file    test_write_failure.c
 * \author  Kamil Kielbasa
 * \brief   A block that will not take a write: retired, repaired, written
 *          off. Flash simulator only.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "flash_stats.h"
#include "partition.h"
#include "suite.h"
#include "workload.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_write_failure);

/* Module interface function definitions ----------------------------------- */

/* Tests: retiring --------------------------------------------------------- */

/*
 * Given: a block holding data, and a flash that has started refusing writes.
 * When:  that block is rewritten.
 * Then:  the write fails, the block is retired and reported, the logical
 *        block still reads what it held, and the next allocation avoids it.
 */
ZTEST(ubi_write_failure, test_a_block_that_refuses_a_write_is_retired)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t kept[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t refused[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(kept, sizeof(kept), 0xBF);
	pattern_fill(refused, sizeof(refused), 0xC0);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, kept, sizeof(kept)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	events_forget();
	flash_fail_writes_after(0);

	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, refused, sizeof(refused)));

	flash_fail_writes_never();

	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD],
		      "the application has to hear about a retired block");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(kept, read, sizeof(kept),
			  "the logical block never moved");

	zassert_ok(ubi_leb_change(ubi, vol_id, 1, kept, sizeof(kept)));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs,
		      "the retired block must not come back on allocation");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an erased and stamped block waiting in the free pool, and a flash
 *        that has started refusing writes.
 * When:  that block is handed out for a logical block that has none yet.
 * Then:  its header does not go down, it is retired and reported without an
 *        erase being tried, and the logical block stays unmapped.
 */
ZTEST(ubi_write_failure, test_a_free_block_that_refuses_its_header_is_retired)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint8_t refused[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(refused, sizeof(refused), 0xC1);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_true(0 < before.free_pebs);

	events_forget();
	flash_ops_forget();
	flash_fail_writes_after(0);

	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, refused, sizeof(refused)));

	flash_fail_writes_never();

	zassert_equal(0, flash_ops("flash_erase_calls"),
		      "a free block is handed out without an erase");
	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);
	zassert_equal(before.free_pebs - 1, after.free_pebs);

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &leb));
	zassert_false(leb.mapped);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a flash that refuses to write over programmed bytes, as internal
 *        flash with ECC does.
 * When:  blocks that still carry headers are erased again, by reclaim and
 *        by a volume table update.
 * Then:  none of them is retired: clearing the headers first is given up
 *        and the erase alone decides, and what was written survives a
 *        reattach.
 */
ZTEST(ubi_write_failure, test_a_flash_that_refuses_overwrites_loses_no_block)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const struct ubi_volume_config other = { .name = "other",
						 .leb_count = 1 };
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t other_id = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xE3);

	/* The first copy is left behind with both headers intact. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &info));

	events_forget();
	flash_refuse_overwrites();

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.reclaimable_pebs, &result));
	zassert_ok(ubi_volume_create(ubi, &other, &other_id));

	flash_faults_clear();

	zassert_equal(0, event_count[UBI_EVENT_PEB_BAD]);
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_find(ubi, other.name, &found));
	zassert_equal(other_id, found);
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: cold blocks worth relocating, and a flash that fails a write part
 *        way through copying one of them.
 * When:  relocation gives up, the cold blocks are erased and the device is
 *        attached again.
 * Then:  they stay erased: the copy relocation gave up on went with the
 *        block it was retired with, rather than stay for the attach to take.
 */
ZTEST(ubi_write_failure, test_a_relocation_that_fails_leaves_no_copy_behind)
{
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0xE4);

	const uint32_t vol_id =
		unevenly_worn_device(cold, sizeof(cold), &leb_count);

	/* The header of the copy goes down whole, its data does not. */
	flash_fail_one_write_after(UBI_HEADER_SIZE + UBI_TEST_WRITE_BLOCK);
	zassert_equal(-EIO, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1,
					    &result));
	flash_faults_clear();

	for (uint32_t lnum = 1; lnum < leb_count; ++lnum)
		zassert_ok(ubi_leb_erase(ubi, vol_id, lnum));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	for (uint32_t lnum = 1; lnum < leb_count; ++lnum) {
		struct ubi_leb_info leb = { 0 };

		zassert_ok(ubi_leb_get_info(ubi, vol_id, lnum, &leb));
		zassert_false(leb.mapped, "block %u came back", lnum);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: another chance --------------------------------------------------- */

/*
 * Given: a block retired by a write fault that has since gone away.
 * When:  a repair runs.
 * Then:  the erase succeeds and the block goes back into service, without
 *        waiting for the next attach.
 */
ZTEST(ubi_write_failure, test_repairing_gives_a_retired_block_another_chance)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x62);

	retire_one_block(vol_id, 0);

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_equal(1, before.bad_pebs);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 0, &result));
	zassert_equal(1, result.remaining, "a retired block is work waiting");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(0, after.bad_pebs);
	zassert_equal(before.free_pebs + 1, after.free_pebs);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: two blocks retired by a flash that is still refusing writes.
 * When:  a repair with a budget of two runs, and then another.
 * Then:  both steps run, because stopping at the first block that cannot be
 *        brought back would leave the rest waiting forever; both blocks are
 *        written off, out of service but no longer work waiting, and not
 *        erased again.
 */
ZTEST(ubi_write_failure, test_a_finished_block_does_not_hold_up_the_others)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	retire_one_block(vol_id, 0);
	retire_one_block(vol_id, 1);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.bad_pebs);

	flash_fail_writes_after(0);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 2, &result),
		   "learning that a block is finished is not a failed repair");
	zassert_equal(2, result.performed);
	zassert_equal(0, result.remaining,
		      "a block written off is no longer work waiting");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(0, result.performed, "and it is not erased again");
	zassert_equal(0, result.remaining);

	flash_fail_writes_never();

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.bad_pebs, "but it is still out of service");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block retired by a one-off write fault.
 * When:  the device is detached and attached again.
 * Then:  the block is in service once more, because retirement lives in RAM
 *        and a one-off fault must not condemn a block forever.
 */
ZTEST(ubi_write_failure, test_a_retired_block_gets_another_chance_on_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };

	retire_one_block(vol_id, 0);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}
