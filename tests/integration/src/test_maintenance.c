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
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Bytes written into a block when a test needs to recognise it later. */
#define PAYLOAD_SIZE (64)

/** Blocks to erase in one go, small enough to leave work behind. */
#define RECLAIM_BUDGET (4)

/* Static function definitions --------------------------------------------- */

/**
 * \brief Blocks currently backing a logical one, derived from the rest.
 */
static uint32_t mapped_pebs(const struct ubi_device_info *info)
{
	return info->peb_count - info->free_pebs - info->reclaimable_pebs -
	       info->bad_pebs;
}

/**
 * \brief Format, attach and create one volume to work in.
 */
static uint32_t volume_ready(uint32_t leb_count)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = leb_count };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	return vol_id;
}

/**
 * \brief Wear one pair of blocks out until the spread is worth acting on.
 *
 *        The volume takes every logical block there is, so only the spare
 *        stays free and the churn has nowhere to spread. That is the shape
 *        wear levelling exists for: one hot block, many cold ones.
 */
static uint32_t wear_out_one_block(uint32_t vol_id)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[PAYLOAD_SIZE];
	uint32_t rounds = 0;

	memset(written, 0x8C, sizeof(written));

	do {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
		zassert_ok(ubi_device_get_info(ubi, &info));

		rounds += 1;
	} while (0 == info.relocatable_pebs && rounds < 4096);

	return info.relocatable_pebs;
}

/**
 * \brief Highest erase count among the blocks behind the cold blocks.
 *
 *        Logical block zero is the one being hammered, so it already sits on
 *        a worn block; the rest are the ones relocation is meant to move.
 */
static uint32_t coldest_leb_wear(uint32_t vol_id, uint32_t leb_count)
{
	struct ubi_leb_info info = { 0 };
	uint32_t worst = 0;

	for (uint32_t lnum = 1; lnum < leb_count; ++lnum) {
		zassert_ok(ubi_leb_get_info(ubi, vol_id, lnum, &info));

		if (info.erase_count > worst)
			worst = info.erase_count;
	}

	return worst;
}

/* Tests: reclaiming ------------------------------------------------------- */

ZTEST(ubi_integration, test_a_budget_of_zero_only_counts_the_work)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 0, &result));

	zassert_equal(0, result.performed, "zero means look, do not touch");
	zassert_equal(info.reclaimable_pebs, result.remaining,
		      "and what it reports has to be what get_info says");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.total_erase_count - info.healthy_pebs,
		      "nothing may have been erased");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_reclaiming_refills_the_free_pool)
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

ZTEST(ubi_integration, test_reclaiming_takes_released_blocks_first)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	memset(written, 0x71, sizeof(written));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	/* The released block still carries the data, so a single step has to
	 * go for it rather than for a block that was never used. */
	zassert_equal(1, count_data_matching(written, sizeof(written)));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(0, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_reclaiming_stops_when_there_is_nothing_left)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	/* A budget larger than the work is not an error. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.peb_count * 2, &result));

	zassert_equal(info.reclaimable_pebs, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 8, &result));
	zassert_equal(0, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: repairing the volume table --------------------------------------- */

ZTEST(ubi_integration, test_repairing_clears_a_degraded_volume_table)
{
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_equal(1, corrupt_volume_tables(ubi, &config, 1));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 0, &result));
	zassert_equal(1, result.remaining, "the pair does not agree yet");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_false(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "the repair has to reach the flash");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_repairing_a_healthy_table_does_nothing)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 4, &result));

	zassert_equal(0, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.global_sqnum, after.global_sqnum,
		      "a healthy pair must not be rewritten");
	zassert_equal(before.revision, after.revision);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: the argument contract -------------------------------------------- */

ZTEST(ubi_integration, test_an_unknown_maintenance_operation_is_refused)
{
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));

	zassert_equal(-EINVAL,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result),
		      "a detached handle has nothing to maintain");

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EINVAL,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, NULL),
		      "and a caller who does not want to know what happened "
		      "has no business asking for it");

	zassert_equal(-EINVAL, ubi_maintenance(ubi, (enum ubi_maintenance_op)99,
					       1, &result));
	zassert_equal(-EINVAL, ubi_maintenance(ubi, (enum ubi_maintenance_op)99,
					       0, &result));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: levelling the wear ----------------------------------------------- */

ZTEST(ubi_integration, test_relocation_moves_cold_data_onto_a_worn_block)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t cold[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	memset(cold, 0x9D, sizeof(cold));

	/* Ending on erased bytes is what a block of ciphertext may look like,
	 * and it is what makes the length worth trimming land off a write
	 * block boundary. */
	memset(&cold[sizeof(cold) - 3], 0xFF, 3);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	wanted.leb_count = before.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum)
		zassert_ok(
			ubi_leb_change(ubi, vol_id, lnum, cold, sizeof(cold)));

	zassert_true(0 < wear_out_one_block(vol_id),
		     "the spread has to grow past the threshold");

	/* Two blocks to choose from, worn very differently: the one the churn
	 * has been hammering, and one a cold block just gave back. Levelling
	 * has to take the worn one. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, wanted.leb_count - 1));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 2, &result));

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_true(before.max_erase_count - before.min_erase_count >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the device has to be unevenly worn to begin with");

	zassert_equal(1, coldest_leb_wear(vol_id, wanted.leb_count),
		      "the cold blocks have been erased exactly once");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(1, result.performed);
	zassert_true(result.remaining < before.relocatable_pebs,
		     "one fewer block should be worth moving");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_true(coldest_leb_wear(vol_id, wanted.leb_count) >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "cold data has to end up on a block that has seen wear");
	zassert_equal(mapped_pebs(&before), mapped_pebs(&after),
		      "one logical block may occupy only one physical one");

	/* The move is invisible: every block still reads what it held. The
	 * last one was unmapped on purpose to free a block. */
	for (uint32_t lnum = 1; lnum < wanted.leb_count - 1; ++lnum) {
		memset(read, 0x00, sizeof(read));
		zassert_ok(
			ubi_leb_read(ubi, vol_id, lnum, 0, read, sizeof(read)));
		zassert_mem_equal(cold, read, sizeof(read),
				  "block %u changed under relocation", lnum);
	}

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(cold, read, sizeof(read),
			  "and the move has to survive a reattach");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_even_device_has_nothing_to_relocate)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	memset(written, 0xAE, sizeof(written));

	for (uint32_t lnum = 0; lnum < 4; ++lnum)
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 8, &result));

	zassert_equal(0, result.performed, "nothing is worn out yet");
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: a block that will not take a write ------------------------------- */

ZTEST(ubi_integration, test_a_block_that_refuses_a_write_is_retired)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t kept[PAYLOAD_SIZE];
	uint8_t refused[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	memset(kept, 0xBF, sizeof(kept));
	memset(refused, 0xC0, sizeof(refused));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, kept, sizeof(kept)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	events_forget();
	flash_fail_writes_after(0);

	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, refused, sizeof(refused)));

	flash_fail_writes_never();

	zassert_true(event_seen[UBI_EVENT_PEB_BAD],
		     "the application has to hear about a retired block");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);

	/* The logical block never moved, so it still reads what it held. */
	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(kept, read, sizeof(kept));

	/* And the retired block must not come back on the next allocation. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, kept, sizeof(kept)));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_repairing_gives_a_retired_block_another_chance)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	memset(written, 0x62, sizeof(written));

	flash_fail_writes_after(0);
	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	flash_fail_writes_never();

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_equal(1, before.bad_pebs);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 0, &result));
	zassert_equal(1, result.remaining, "a retired block is work waiting");

	/* The fault was a one-off, so the erase succeeds and the block goes
	 * back into service rather than waiting for the next attach. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(0, after.bad_pebs);
	zassert_equal(before.free_pebs + 1, after.free_pebs);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_repairing_writes_off_a_block_that_stays_broken)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	memset(written, 0x73, sizeof(written));

	flash_fail_writes_after(0);

	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	/*
	 * Still refusing writes, so the block cannot come back. Learning that
	 * is an answer, not a failure of the repair: a part wearing out is
	 * what this device is expected to live through.
	 */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));

	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining,
		      "a block written off is no longer work waiting");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_pebs, "but it is still out of service");

	/* And it must not cost another erase every time repairs are asked
	 * for. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 4, &result));
	zassert_equal(0, result.performed);
	zassert_equal(0, result.remaining);

	flash_fail_writes_never();

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_finished_block_does_not_hold_up_the_others)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	memset(written, 0x84, sizeof(written));

	flash_fail_writes_after(0);

	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 1, written, sizeof(written)));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.bad_pebs);

	/* Both steps have to run. Stopping at the first block that cannot be
	 * brought back would leave every block behind it waiting forever. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 2, &result));

	zassert_equal(2, result.performed);
	zassert_equal(0, result.remaining);

	flash_fail_writes_never();

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_retired_block_gets_another_chance_on_reattach)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info info = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	memset(written, 0xD1, sizeof(written));

	flash_fail_writes_after(0);
	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	flash_fail_writes_never();

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	/* Retirement lives in RAM, so a one-off fault does not condemn a
	 * block forever; a lasting one will retire it again. */
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_relocation_leaves_room_to_append_afterwards)
{
	struct ubi_device_info info = { 0 };
	struct ubi_leb_info leb = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t first[PAYLOAD_SIZE];
	uint8_t second[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	memset(first, 0xE2, sizeof(first));
	memset(second, 0xF3, sizeof(second));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	/* Appended, not changed, so no header claims how far the data goes
	 * and the rest of the block is still erased and writable. */
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, 0, first, sizeof(first)));

	for (uint32_t lnum = 2; lnum < wanted.leb_count; ++lnum)
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, first,
					  sizeof(first)));

	zassert_true(0 < wear_out_one_block(vol_id));

	/* Move blocks about until the appended one has travelled. */
	for (uint32_t round = 0; round < 256; ++round) {
		zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &leb));

		if (leb.erase_count > CONFIG_UBI_WEAR_LEVELING_THRESHOLD)
			break;

		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1,
					   &result));
	}

	zassert_true(leb.erase_count > CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the appended block has to have been relocated");

	/*
	 * Relocation sealed the block, and a seal covers only what was really
	 * written. Had it covered the erased tail too, this append would put
	 * the block beyond repair at the next attach.
	 */
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, sizeof(first), second,
				    sizeof(second)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(first, read, sizeof(first));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, sizeof(first), read,
				sizeof(read)));
	zassert_mem_equal(second, read, sizeof(second));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_relocation_refuses_a_block_it_cannot_vouch_for)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t cold[PAYLOAD_SIZE];

	memset(cold, 0xA7, sizeof(cold));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	wanted.leb_count = before.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum)
		zassert_ok(
			ubi_leb_change(ubi, vol_id, lnum, cold, sizeof(cold)));

	zassert_true(0 < wear_out_one_block(vol_id));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, wanted.leb_count - 1));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 2, &result));
	zassert_ok(ubi_device_get_info(ubi, &before));

	/* Rot reaches the data after the block was sealed. Copying it now
	 * would put a fresh, correct checksum on corrupted bytes. */
	zassert_true(0 < corrupt_data_matching(cold, sizeof(cold)));

	events_forget();

	zassert_equal(-EBADMSG, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
						1, &result));

	zassert_equal(0, result.performed);
	zassert_true(event_seen[UBI_EVENT_PEB_BAD]);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs,
		      "the block it would not move has to be retired");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_relocation_refuses_a_block_with_a_broken_header)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t cold[PAYLOAD_SIZE];

	memset(cold, 0xB8, sizeof(cold));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	wanted.leb_count = before.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum)
		zassert_ok(
			ubi_leb_change(ubi, vol_id, lnum, cold, sizeof(cold)));

	zassert_true(0 < wear_out_one_block(vol_id));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, wanted.leb_count - 1));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 2, &result));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_true(0 < corrupt_header_of_data_matching(cold, sizeof(cold)));

	zassert_equal(-EBADMSG, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
						1, &result));

	zassert_equal(0, result.performed);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}
