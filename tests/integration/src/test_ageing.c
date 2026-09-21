/**
 * \file    test_ageing.c
 * \author  Kamil Kielbasa
 * \brief   What a long life does to the device: where the wear ends up.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Bytes written on every pass, small enough that the cost is the erase. */
#define AGEING_PAYLOAD_SIZE (64)

/** Logical blocks left unclaimed, so few blocks carry the whole traffic. */
#define AGEING_FREE_LEBS (4)

/** Rewrites a long life stands for, enough to leave the threshold no slack. */
#define AGEING_ROUNDS (32768)

/** Rewrites used to wear a small set of blocks before a pool is disturbed. */
#define SETTLE_ROUNDS (8192)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Fill a volume so that only a handful of blocks are left to rotate.
 *
 * \return How many logical blocks it holds.
 */
static uint32_t volume_under_load(uint32_t *vol_id);

/**
 * \brief Rewrite one logical block over and over, keeping maintenance up.
 */
static void churn(uint32_t vol_id, uint32_t rounds, bool level);

/**
 * \brief Read the volume back and insist on finding every block whole.
 *
 *        Block zero is the one the churn keeps rewriting and is skipped.
 */
static void volume_verify(uint32_t vol_id, uint32_t leb_count);

/**
 * \brief How worn the block carrying a logical block is.
 */
static uint32_t leb_wear(uint32_t vol_id, uint32_t lnum);

/**
 * \brief Bring a device up with its free pool already erased and stamped.
 *
 * \return How many logical blocks the volume holds.
 */
static uint32_t device_ready(uint32_t *vol_id);

/* Static function definitions --------------------------------------------- */

static uint32_t volume_under_load(uint32_t *vol_id)
{
	struct ubi_volume_config wanted = { .name = "wear" };
	struct ubi_device_info info = { 0 };
	uint8_t written[AGEING_PAYLOAD_SIZE];

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_true(info.free_lebs > AGEING_FREE_LEBS);
	wanted.leb_count = info.free_lebs - AGEING_FREE_LEBS;

	zassert_ok(ubi_volume_create(ubi, &wanted, vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum) {
		memset(written, (uint8_t)lnum, sizeof(written));
		zassert_ok(ubi_leb_change(ubi, *vol_id, lnum, written,
					  sizeof(written)));
	}

	return wanted.leb_count;
}

static void churn(uint32_t vol_id, uint32_t rounds, bool level)
{
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[AGEING_PAYLOAD_SIZE];

	memset(written, 0x00, sizeof(written));

	for (uint32_t round = 0; round < rounds; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 2,
					   &result));

		if (level)
			zassert_ok(ubi_maintenance(
				ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	}
}

static void volume_verify(uint32_t vol_id, uint32_t leb_count)
{
	uint8_t expected[AGEING_PAYLOAD_SIZE];
	uint8_t read[AGEING_PAYLOAD_SIZE];

	for (uint32_t lnum = 1; lnum < leb_count; ++lnum) {
		memset(expected, (uint8_t)lnum, sizeof(expected));
		memset(read, 0xAA, sizeof(read));

		zassert_ok(
			ubi_leb_read(ubi, vol_id, lnum, 0, read, sizeof(read)));
		zassert_mem_equal(expected, read, sizeof(read),
				  "block %u lost its contents", lnum);
	}
}

static uint32_t leb_wear(uint32_t vol_id, uint32_t lnum)
{
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_get_info(ubi, vol_id, lnum, &info));

	return info.erase_count;
}

static uint32_t device_ready(uint32_t *vol_id)
{
	struct ubi_maintenance_result result = { 0 };
	const uint32_t leb_count = volume_under_load(vol_id);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 512, &result));
	zassert_equal(0, result.remaining, "the free pool has to be ready");

	return leb_count;
}

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_integration, test_levelling_keeps_the_wear_within_its_threshold)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	const uint32_t leb_count = volume_under_load(&vol_id);

	zassert_ok(ubi_device_get_info(ubi, &before));

	churn(vol_id, AGEING_ROUNDS, true);

	zassert_ok(ubi_device_get_info(ubi, &after));

	/* Every block the cold data was sitting on has been handed over and
	 * has taken its turn under the traffic. */
	zassert_true(after.min_erase_count > before.min_erase_count,
		     "no block may be left out of the rotation");

	zassert_true(after.max_erase_count - after.min_erase_count <=
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "wear ran from %u to %u", after.min_erase_count,
		     after.max_erase_count);

	/* And none of that moving about may have cost a byte. */
	volume_verify(vol_id, leb_count);
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	volume_verify(vol_id, leb_count);
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_without_levelling_the_cold_blocks_never_take_a_turn)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_true(0 < volume_under_load(&vol_id));

	zassert_ok(ubi_device_get_info(ubi, &before));

	churn(vol_id, AGEING_ROUNDS, false);

	zassert_ok(ubi_device_get_info(ubi, &after));

	/* Reclaim alone hands the same few blocks back and forth, so the ones
	 * holding data that never changes are never asked to do anything. */
	zassert_equal(before.min_erase_count, after.min_erase_count,
		      "the cold blocks must have been passed over entirely");

	zassert_true(after.max_erase_count - after.min_erase_count >
			     4 * CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "wear ran from %u to %u, which levelling would not allow",
		     after.min_erase_count, after.max_erase_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what an operation costs ------------------------------------------ */

ZTEST(ubi_integration, test_attach_reads_the_blocks_and_writes_nothing)
{
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_true(0 < volume_under_load(&vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	flash_ops_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	/* Attach is two passes over the headers and one over the data each
	 * sealed block promised, and it decides everything else in memory. */
	zassert_true(flash_ops("flash_read_calls") <= 3 * info.peb_count,
		     "attach read %u times over %u blocks",
		     flash_ops("flash_read_calls"), info.peb_count);

	zassert_equal(0, flash_ops("flash_write_calls"));
	zassert_equal(0, flash_ops("flash_erase_calls"));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_change_on_a_ready_device_costs_no_erase)
{
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[AGEING_PAYLOAD_SIZE];

	memset(written, 0x5C, sizeof(written));

	zassert_true(0 < device_ready(&vol_id));

	flash_ops_forget();
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	/* Keeping maintenance up is what buys this: the block was erased and
	 * stamped long before the write needed it. */
	zassert_equal(0, flash_ops("flash_erase_calls"),
		      "the erase is maintenance's to pay, not the writer's");

	zassert_equal(2, flash_ops("flash_write_calls"),
		      "one sealed header and one payload");

	zassert_equal(0, flash_ops("flash_read_calls"),
		      "where a logical block lives is known in memory");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_read_goes_straight_to_the_block)
{
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t read[AGEING_PAYLOAD_SIZE];

	zassert_true(0 < device_ready(&vol_id));

	flash_ops_forget();
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));

	zassert_equal(IS_ENABLED(CONFIG_UBI_VERIFY_ON_READ) ? 2 : 1,
		      flash_ops("flash_read_calls"),
		      "checking the seal first is the one thing that adds a "
		      "read");

	zassert_equal(0, flash_ops("flash_write_calls"));
	zassert_equal(0, flash_ops("flash_erase_calls"));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_reclaiming_costs_one_erase_and_one_header)
{
	struct ubi_maintenance_result result = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[AGEING_PAYLOAD_SIZE];

	memset(written, 0x6D, sizeof(written));

	zassert_true(0 < device_ready(&vol_id));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	flash_ops_forget();
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));

	zassert_equal(1, result.performed);
	zassert_equal(1, flash_ops("flash_erase_calls"));
	zassert_equal(1, flash_ops("flash_write_calls"),
		      "the erase counter header is all that goes back down");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_replacing_a_lost_volume_table_copy_costs_one_erase)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	/* Wipe the block holding the second copy, headers and all, so the
	 * repair has to find a new block rather than reuse the old one. */
	stamp_erase_count(config.ikm_key_id, 1, info.image_seq, 1);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		     "a copy is missing, so the pair is degraded");

	flash_ops_forget();
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);

	/* One for the surviving copy, which is reused in place. The
	 * replacement comes off the free pool erased already. */
	zassert_equal(1, flash_ops("flash_erase_calls"),
		      "a commit must not erase a block it was handed erased");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_levelling_moves_the_block_with_the_most_life_left)
{
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[AGEING_PAYLOAD_SIZE];

	memset(written, 0x2E, sizeof(written));

	zassert_true(0 < volume_under_load(&vol_id));

	/* Rewriting a block once the rotation has some wear on it puts that
	 * block on a part-worn carrier instead of a fresh one. */
	churn(vol_id, 2048, false);
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, written, sizeof(written)));

	const uint32_t part_worn = leb_wear(vol_id, 1);

	zassert_true(part_worn > leb_wear(vol_id, 2),
		     "block 1 has to be the more worn of the two");

	/* Wear the rotation on until even that block is worth moving, so the
	 * choice is between two candidates rather than one. */
	churn(vol_id, 8192, false);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.max_erase_count - part_worn >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "both have to be worth moving for the choice to matter");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);

	/* Blocks with more life left go first: spending the part-worn one now
	 * would buy less and cost the same. */
	zassert_equal(part_worn, leb_wear(vol_id, 1),
		      "the part-worn block has to wait its turn");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_levelling_lets_a_fresh_write_settle_first)
{
	struct ubi_volume_config cold = { .name = "cold" };
	struct ubi_volume_config hot = { .name = "hot", .leb_count = 8 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t cold_id = UBI_VOL_ID_INVALID;
	uint32_t hot_id = UBI_VOL_ID_INVALID;
	uint32_t relocations = 0;
	uint8_t written[AGEING_PAYLOAD_SIZE];

	memset(written, 0x77, sizeof(written));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	cold.leb_count = info.free_lebs - hot.leb_count - 4;
	zassert_ok(ubi_volume_create(ubi, &cold, &cold_id));
	zassert_ok(ubi_volume_create(ubi, &hot, &hot_id));

	for (uint32_t lnum = 0; lnum < cold.leb_count; ++lnum)
		zassert_ok(ubi_leb_change(ubi, cold_id, lnum, written,
					  sizeof(written)));

	/* Wear the few blocks the hot volume rotates through, hard. */
	for (uint32_t round = 0; round < SETTLE_ROUNDS; ++round) {
		zassert_ok(ubi_leb_change(ubi, hot_id, round % hot.leb_count,
					  written, sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 2,
					   &result));
	}

	/* Then hand the cold volume's blocks back, so a pool of hammered
	 * blocks is suddenly flooded with barely used ones. */
	zassert_ok(ubi_volume_remove(ubi, cold_id));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 512, &result));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.max_erase_count - info.min_erase_count >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "every fresh block has to look worth moving");

	flash_ops_forget();

	for (uint32_t round = 0; round < SETTLE_ROUNDS / 2; ++round) {
		zassert_ok(ubi_leb_change(ubi, hot_id, round % hot.leb_count,
					  written, sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 2,
					   &result));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1,
					   &result));
		relocations += result.performed;
	}

	/*
	 * Those blocks are the least worn in use, so levelling wants every
	 * one of them. Carrying them off would be wasted: this is the volume
	 * being rewritten, and the data will be gone in a few rounds anyway.
	 */
	zassert_true(relocations < SETTLE_ROUNDS / 64,
		     "hot data was carried off %u times in %u rounds",
		     relocations, SETTLE_ROUNDS / 2);

	zassert_true(flash_ops("flash_erase_calls") < SETTLE_ROUNDS,
		     "%u erases for %u rewrites is the cost of shuttling",
		     flash_ops("flash_erase_calls"), SETTLE_ROUNDS / 2);

	zassert_ok(ubi_device_deinit(ubi));
}
