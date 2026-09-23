/**
 * \file    test_maintenance.c
 * \author  Kamil Kielbasa
 * \brief   Reclaiming, relocating and repairing, on the application's clock.
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
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Blocks to erase in one go, small enough to leave work behind. */
#define RECLAIM_BUDGET (4)

/** Rounds of churn after which the hot pair stands one erase past the
 *  threshold above the cold blocks; the pair takes the erases in turn. */
#define WEAR_OUT_ROUNDS (2 * (CONFIG_UBI_WEAR_LEVELING_THRESHOLD + 1))

/** How far above the least worn free block relocation reaches for a target,
 *  as the help of CONFIG_UBI_WEAR_LEVELING_THRESHOLD states. */
#define RELOCATION_REACH (2 * CONFIG_UBI_WEAR_LEVELING_THRESHOLD)

/** Erased bytes cold data ends on, which relocation trims. */
#define ERASED_TAIL (3)

/* Trimming the tail has to land off a write block boundary. */
BUILD_ASSERT(1 == UBI_TEST_WRITE_BLOCK || ERASED_TAIL < UBI_TEST_WRITE_BLOCK);

/** The first value past the last operation ubi.h defines. */
#define OPERATION_UNKNOWN \
	((enum ubi_maintenance_op)(UBI_MAINTENANCE_REPAIR + 1))

/* Static function declarations -------------------------------------------- */

/**
 * \brief Rewrite logical block zero until the cold blocks are worth
 *        relocating.
 *
 *        Every other block is mapped, so the churn stays on one pair of
 *        physical blocks.
 */
static void wear_out_one_block(uint32_t vol_id);

/**
 * \brief Fill every logical block with \p cold, wear block zero out, and give
 *        the last block back so relocation has a choice to make.
 *
 * \param[in] cold                      Bytes every cold block holds.
 * \param length                        How many.
 * \param[out] leb_count                Logical blocks the volume holds.
 *
 * \return Identifier the volume was given.
 */
static uint32_t unevenly_worn_device(const uint8_t *cold, size_t length,
				     uint32_t *leb_count);

/**
 * \brief Highest erase count behind logical blocks 1 to \p leb_count - 1.
 */
static uint32_t cold_wear_max(uint32_t vol_id, uint32_t leb_count);

/**
 * \brief Fail unless logical blocks 1 to \p leb_count - 2 hold \p cold.
 *
 *        The last one was given back by unevenly_worn_device().
 */
static void cold_blocks_check(uint32_t vol_id, uint32_t leb_count,
			      const uint8_t *cold, size_t length);

#if defined(CONFIG_FLASH_SIMULATOR)

/**
 * \brief Retire one block by refusing the write that would have landed on it.
 */
static void retire_one_block(uint32_t vol_id, uint32_t lnum);

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_maintenance);

/* Static function definitions --------------------------------------------- */

static void wear_out_one_block(uint32_t vol_id)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x8C);

	for (uint32_t round = 0; round < WEAR_OUT_ROUNDS; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
		zassert_equal(1, result.performed);
	}

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(0 < info.relocatable_pebs,
		     "the spread has to grow past the threshold");
}

static uint32_t unevenly_worn_device(const uint8_t *cold, size_t length,
				     uint32_t *leb_count)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum)
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, cold, length));

	wear_out_one_block(vol_id);

	/* Next to the worn spare, the block a cold one gives back. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, wanted.leb_count - 1));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	*leb_count = wanted.leb_count;

	return vol_id;
}

static uint32_t cold_wear_max(uint32_t vol_id, uint32_t leb_count)
{
	uint32_t worst = 0;

	for (uint32_t lnum = 1; lnum < leb_count; ++lnum)
		worst = MAX(worst, leb_wear(vol_id, lnum));

	return worst;
}

static void cold_blocks_check(uint32_t vol_id, uint32_t leb_count,
			      const uint8_t *cold, size_t length)
{
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_equal(sizeof(read), length);

	for (uint32_t lnum = 1; lnum < leb_count - 1; ++lnum) {
		memset(read, 0x00, sizeof(read));
		zassert_ok(
			ubi_leb_read(ubi, vol_id, lnum, 0, read, sizeof(read)));
		zassert_mem_equal(cold, read, sizeof(read),
				  "block %u changed under relocation", lnum);
	}
}

#if defined(CONFIG_FLASH_SIMULATOR)

static void retire_one_block(uint32_t vol_id, uint32_t lnum)
{
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x5E);

	flash_fail_writes_after(0);
	zassert_equal(-EIO, ubi_leb_change(ubi, vol_id, lnum, written,
					   sizeof(written)));
	flash_fail_writes_never();
}

#endif /* CONFIG_FLASH_SIMULATOR */

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
	zassert_equal(before.global_sqnum, after.global_sqnum,
		      "a healthy pair must not be rewritten");
	zassert_equal(before.revision, after.revision);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: the argument contract -------------------------------------------- */

/*
 * Given: a detached handle, and then an attached one.
 * When:  maintenance is asked for without a handle, without a result, or
 *        with an operation that does not exist.
 * Then:  each is refused as a bad argument.
 */
ZTEST(ubi_maintenance, test_an_unknown_maintenance_operation_is_refused)
{
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));

	zassert_equal(-EINVAL,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result),
		      "a detached handle has nothing to maintain");

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

/* Tests: levelling the wear ----------------------------------------------- */

/*
 * Given: an unevenly worn device, with cold data on barely used blocks and
 *        one block the churn has been hammering.
 * When:  one relocation step runs.
 * Then:  cold data ends up on the worn block, nothing is lost or duplicated,
 *        and the move survives a reattach.
 */
ZTEST(ubi_maintenance, test_relocation_moves_cold_data_onto_a_worn_block)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0x9D);
	memset(&cold[sizeof(cold) - ERASED_TAIL], UBI_TEST_ERASED, ERASED_TAIL);

	const uint32_t vol_id =
		unevenly_worn_device(cold, sizeof(cold), &leb_count);

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_true(before.max_erase_count - before.min_erase_count >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the device has to be unevenly worn to begin with");
	zassert_equal(1, cold_wear_max(vol_id, leb_count),
		      "the cold blocks have been erased exactly once");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining, "the one worn free block is taken");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_true(cold_wear_max(vol_id, leb_count) >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "cold data has to end up on a block that has seen wear");
	zassert_equal(mapped_pebs(&before), mapped_pebs(&after),
		      "one logical block may occupy only one physical one");
	cold_blocks_check(vol_id, leb_count, cold, sizeof(cold));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_true(cold_wear_max(vol_id, leb_count) >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the move has to survive a reattach");
	cold_blocks_check(vol_id, leb_count, cold, sizeof(cold));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose blocks are all worn about the same.
 * When:  relocation is offered a budget.
 * Then:  it does nothing, because nothing is worth moving yet.
 */
ZTEST(ubi_maintenance, test_an_even_device_has_nothing_to_relocate)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xAE);

	for (uint32_t lnum = 0; lnum < UBI_TEST_VOLUME_LEBS; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));
	}

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(0, result.performed, "nothing is worn out yet");
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: one free block worn exactly as far above the least worn free block
 *        as relocation may reach.
 * When:  relocation is offered a budget.
 * Then:  it does nothing, because that block is out of reach and moving cold
 *        data there would spend the last of it.
 */
ZTEST(ubi_maintenance, test_a_block_too_far_gone_is_not_relocated_onto)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xC3);

	for (uint32_t lnum = 0; lnum < UBI_TEST_VOLUME_LEBS; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));
	}

	/* Free blocks to choose from next to the worn one. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, RECLAIM_BUDGET,
				   &result));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	const uint32_t far_gone = info.min_erase_count + RELOCATION_REACH;

	stamp_erase_count(config.ikm_key_id, info.peb_count - 1, info.image_seq,
			  far_gone);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(far_gone, info.max_erase_count,
		      "the stamped block has to be the worn one");
	zassert_equal(RECLAIM_BUDGET + 1, info.free_pebs);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(0, result.performed,
		      "the only worn block is out of reach");
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: a block that will not take a write ------------------------------- */

/* Fault injection exists only on the flash simulator. */
#if defined(CONFIG_FLASH_SIMULATOR)

/*
 * Given: a block holding data, and a flash that has started refusing writes.
 * When:  that block is rewritten.
 * Then:  the write fails, the block is retired and reported, the logical
 *        block still reads what it held, and the next allocation avoids it.
 */
ZTEST(ubi_maintenance, test_a_block_that_refuses_a_write_is_retired)
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
 * Given: a block retired by a write fault that has since gone away.
 * When:  a repair runs.
 * Then:  the erase succeeds and the block goes back into service, without
 *        waiting for the next attach.
 */
ZTEST(ubi_maintenance, test_repairing_gives_a_retired_block_another_chance)
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
 * Given: a retired block on a flash that is still refusing writes.
 * When:  a repair runs, twice.
 * Then:  the block is written off rather than retried forever: it stays out
 *        of service but stops counting as work waiting.
 */
ZTEST(ubi_maintenance, test_repairing_writes_off_a_block_that_stays_broken)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	retire_one_block(vol_id, 0);

	flash_fail_writes_after(0);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result),
		   "learning that a block is finished is not a failed repair");

	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining,
		      "a block written off is no longer work waiting");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_pebs, "but it is still out of service");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(0, result.performed, "and it is not erased again");
	zassert_equal(0, result.remaining);

	flash_fail_writes_never();

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: two blocks retired by a flash that is still refusing writes.
 * When:  a repair with a budget of two runs.
 * Then:  both steps run, because stopping at the first block that cannot be
 *        brought back would leave every block behind it waiting forever.
 */
ZTEST(ubi_maintenance, test_a_finished_block_does_not_hold_up_the_others)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	retire_one_block(vol_id, 0);
	retire_one_block(vol_id, 1);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.bad_pebs);

	flash_fail_writes_after(0);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 2, &result));

	zassert_equal(2, result.performed);
	zassert_equal(0, result.remaining);

	flash_fail_writes_never();

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a released block and a flash that has stopped taking erases, which
 *        is how a NOR part usually announces that it is finished.
 * When:  a reclaim step tries to erase it.
 * Then:  the step fails rather than reporting work it did not do, the block
 *        is retired and reported, and a repair brings it back once the fault
 *        has gone.
 */
ZTEST(ubi_maintenance, test_a_block_that_refuses_an_erase_is_retired)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x95);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	events_forget();
	flash_fail_erases_after(0);

	zassert_equal(-EIO, ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					    &result));
	zassert_equal(0, result.performed,
		      "a step that failed is not a step performed");

	flash_fail_erases_never();

	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD],
		      "the application has to hear about a retired block");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs, after.bad_pebs,
		      "the fault was a one-off, so the block goes back");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block retired by a one-off write fault.
 * When:  the device is detached and attached again.
 * Then:  the block is in service once more, because retirement lives in RAM
 *        and a one-off fault must not condemn a block forever.
 */
ZTEST(ubi_maintenance, test_a_retired_block_gets_another_chance_on_reattach)
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

#endif /* CONFIG_FLASH_SIMULATOR */

/*
 * Given: a block written by an append, so no header claims how far its data
 *        goes, which relocation has since carried to another block.
 * When:  the caller appends to it again.
 * Then:  the write goes through, because the seal relocation put on it
 *        covers only what was really written.
 */
ZTEST(ubi_maintenance, test_relocation_leaves_room_to_append_afterwards)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0xE2);
	pattern_fill(second, sizeof(second), 0xF3);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, 0, first, sizeof(first)));

	for (uint32_t lnum = 2; lnum < wanted.leb_count; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, first,
					  sizeof(first)));
	}

	wear_out_one_block(vol_id);

	/* Of the equally cold blocks, relocation takes the lowest numbered,
	 * and block 1 was the first one written. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);
	zassert_true(leb_wear(vol_id, 1) > CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the appended block has to have been relocated");

	/* A seal over the erased tail as well would condemn this append at
	 * the next attach. */
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, sizeof(first), second,
				    sizeof(second)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(first, read, sizeof(first));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, sizeof(first), read,
				sizeof(read)));
	zassert_mem_equal(second, read, sizeof(second));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block whose data rotted after it was sealed.
 * When:  relocation tries to move it.
 * Then:  it refuses and retires the block, because copying now would put a
 *        fresh, correct checksum on corrupted bytes.
 */
ZTEST(ubi_maintenance, test_relocation_refuses_a_block_it_cannot_vouch_for)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0xA7);

	unevenly_worn_device(cold, sizeof(cold), &leb_count);
	zassert_ok(ubi_device_get_info(ubi, &before));

	/* Which cold block relocation reaches for is not this test's to
	 * decide, so the rot reaches every one of them. */
	const uint32_t holding = count_data_matching(cold, sizeof(cold));

	zassert_true(0 < holding);
	zassert_equal(holding, corrupt_data_matching(cold, sizeof(cold)));

	events_forget();

	zassert_equal(-EBADMSG, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
						1, &result));

	zassert_equal(0, result.performed);
	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs,
		      "the block it would not move has to be retired");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block whose volume identifier header was damaged.
 * When:  relocation tries to move it.
 * Then:  it refuses and retires the block, because it cannot vouch for what
 *        the block claims to hold.
 */
ZTEST(ubi_maintenance, test_relocation_refuses_a_block_with_a_broken_header)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0xB8);

	unevenly_worn_device(cold, sizeof(cold), &leb_count);
	zassert_ok(ubi_device_get_info(ubi, &before));

	const uint32_t holding = count_data_matching(cold, sizeof(cold));

	zassert_true(0 < holding);
	zassert_equal(holding,
		      corrupt_header_of_data_matching(cold, sizeof(cold)));

	events_forget();

	zassert_equal(-EBADMSG, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
						1, &result));

	zassert_equal(0, result.performed);
	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}
