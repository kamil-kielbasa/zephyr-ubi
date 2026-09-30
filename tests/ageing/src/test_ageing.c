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
#include <inttypes.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_stats.h"
#include "forge.h"
#include "partition.h"
#include "suite.h"
#include "table_copies.h"

/* Module defines ---------------------------------------------------------- */

/** Seed of what churn() writes. */
#define CHURN_SEED (0xF0)

/** Blocks a round may release: the rewritten block's old copy, and the
 *  block relocation moved off. */
#define RECLAIM_PER_ROUND (2)

/** Relocation steps a round takes with levelling on, and with it off. */
#define LEVELLING_ON (1)
#define LEVELLING_OFF (0)

/** Logical blocks the hot volume of the protection test rotates through. */
#define HOT_LEBS (8)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Rewrite logical block zero \p rounds times, reclaiming what each
 *        round releases and taking \p relocations relocation steps a round.
 */
static void churn(uint32_t vol_id, uint32_t rounds, uint32_t relocations);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_ageing);

/* Static function definitions --------------------------------------------- */

static void churn(uint32_t vol_id, uint32_t rounds, uint32_t relocations)
{
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), CHURN_SEED);

	for (uint32_t round = 0; round < rounds; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
					   RECLAIM_PER_ROUND, &result));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
					   relocations, &result));
	}
}

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a volume filled so that only a handful of blocks are free to rotate.
 * When:  one block is rewritten a lifetime over with levelling on.
 * Then:  every block took its turn, the spread stays inside the threshold,
 *        and not a byte of the cold data was lost to all that moving about.
 */
ZTEST(ubi_ageing, test_levelling_keeps_the_wear_within_its_threshold)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	const uint32_t leb_count = pool_ready(&vol_id);

	zassert_ok(ubi_device_get_info(ubi, &before));

	churn(vol_id, CONFIG_UBI_TEST_LIFETIME_ROUNDS, LEVELLING_ON);

	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_true(after.min_erase_count > before.min_erase_count,
		     "no block may be left out of the rotation");
	zassert_true(after.max_erase_count - after.min_erase_count <=
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "wear ran from %u to %u", after.min_erase_count,
		     after.max_erase_count);

	volume_check(vol_id, 1, leb_count, UBI_TEST_LOAD_SEED);
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	volume_check(vol_id, 1, leb_count, UBI_TEST_LOAD_SEED);
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: the same volume under the same traffic, with levelling off.
 * When:  one block is rewritten a lifetime over.
 * Then:  the cold blocks are passed over entirely and the spread runs past
 *        the threshold, so the test above had a spread to level.
 */
ZTEST(ubi_ageing, test_without_levelling_the_cold_blocks_never_take_a_turn)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_true(0 < pool_ready(&vol_id));
	zassert_ok(ubi_device_get_info(ubi, &before));

	churn(vol_id, CONFIG_UBI_TEST_LIFETIME_ROUNDS, LEVELLING_OFF);

	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.min_erase_count, after.min_erase_count,
		      "the cold blocks must have been passed over entirely");
	zassert_true(after.max_erase_count - after.min_erase_count >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "wear ran from %u to %u, which levelling would not allow",
		     after.min_erase_count, after.max_erase_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: two candidates worth relocating, one part-worn and one barely used.
 * When:  a single relocation step runs.
 * Then:  the barely used block goes first, because spending the part-worn one
 *        now would buy less life and cost the same erase.
 */
ZTEST(ubi_ageing, test_levelling_moves_the_block_with_the_most_life_left)
{
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_true(0 < pool_ready(&vol_id));

	/* A rewrite once the rotation has some wear lands on a part-worn
	 * block rather than a fresh one. */
	churn(vol_id, CONFIG_UBI_TEST_CHOICE_ROUNDS, LEVELLING_OFF);

	leb_payload(UBI_TEST_LOAD_SEED, 1, written, sizeof(written));
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, written, sizeof(written)));

	const uint32_t part_worn = leb_wear(vol_id, 1);

	zassert_true(part_worn > leb_wear(vol_id, 2),
		     "block 1 has to be the more worn of the two");

	churn(vol_id, CONFIG_UBI_TEST_CHOICE_ROUNDS, LEVELLING_OFF);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.max_erase_count - part_worn >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "both have to be worth moving for the choice to matter");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);

	zassert_equal(part_worn, leb_wear(vol_id, 1),
		      "the part-worn block has to wait its turn");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: hot blocks worn hard, and then a flood of barely used free blocks
 *        that makes every hot block look worth relocating.
 * When:  the hot volume carries on being rewritten with levelling on.
 * Then:  levelling moves no block but a volume table copy, and every other
 *        erase is one a rewrite asked for.
 */
ZTEST(ubi_ageing, test_levelling_leaves_freshly_written_blocks_alone)
{
	struct ubi_volume_config cold = { .name = "cold", .leb_count = 0 };
	const struct ubi_volume_config hot = { .name = "hot",
					       .leb_count = HOT_LEBS };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t tables_before[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t tables_after[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t cold_id = UBI_VOL_ID_INVALID;
	uint32_t hot_id = UBI_VOL_ID_INVALID;
	uint32_t relocations = 0;
	uint32_t tables_moved = 0;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	cold.leb_count =
		info.free_lebs - hot.leb_count - UBI_TEST_LOAD_FREE_LEBS;
	zassert_ok(ubi_volume_create(ubi, &cold, &cold_id));
	zassert_ok(ubi_volume_create(ubi, &hot, &hot_id));

	for (uint32_t lnum = 0; lnum < cold.leb_count; ++lnum) {
		leb_payload(UBI_TEST_LOAD_SEED, lnum, written, sizeof(written));
		zassert_ok(ubi_leb_change(ubi, cold_id, lnum, written,
					  sizeof(written)));
	}

	pattern_fill(written, sizeof(written), 0xF1);

	for (uint32_t round = 0; round < CONFIG_UBI_TEST_PROTECTION_ROUNDS;
	     ++round) {
		zassert_ok(ubi_leb_change(ubi, hot_id, round % HOT_LEBS,
					  written, sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
					   RECLAIM_PER_ROUND, &result));
	}

	/* Removing the cold volume floods the pool with barely used blocks. */
	zassert_ok(ubi_volume_remove(ubi, cold_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.reclaimable_pebs, &result));
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.max_erase_count - info.min_erase_count >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "every hot block has to look worth moving");

	const uint64_t erases_before = info.total_erase_count;

	volume_table_copy_blocks(config.ikm_key_id, tables_before);

	for (uint32_t round = 0; round < CONFIG_UBI_TEST_PROTECTION_ROUNDS;
	     ++round) {
		zassert_ok(ubi_leb_change(ubi, hot_id, round % HOT_LEBS,
					  written, sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
					   RECLAIM_PER_ROUND, &result));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
					   LEVELLING_ON, &result));
		relocations += result.performed;
	}

	volume_table_copy_blocks(config.ikm_key_id, tables_after);

	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum) {
		if (tables_before[lnum] != tables_after[lnum])
			tables_moved += 1;
	}

	zassert_equal(tables_moved, relocations, "hot data was carried off");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(CONFIG_UBI_TEST_PROTECTION_ROUNDS + relocations,
		      info.total_erase_count - erases_before,
		      "an erase no rewrite asked for is the cost of shuttling");

	/* The copies it moved are the only ones there are. */
	zassert_ok(ubi_device_deinit(ubi));
	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(0, event_count[UBI_EVENT_VOLUME_TABLE_CORRUPT]);
	zassert_equal(0, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED]);
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count,
		      "the volume table has to survive being moved");
	zassert_ok(ubi_volume_find(ubi, hot.name, &hot_id));

	zassert_ok(ubi_device_deinit(ubi));
}

#if defined(CONFIG_FLASH_SIMULATOR)

/*
 * Given: a loaded volume on the flash simulator, which counts the erases
 *        every block takes.
 * When:  one block is rewritten a lifetime over with levelling on.
 * Then:  what each erase counter header gained is exactly what the flash
 *        took, the device's total and the rewritten block's count say the
 *        same, and the counts on the flash stay within the threshold.
 */
ZTEST(ubi_ageing, test_the_wear_ubi_records_is_the_wear_the_flash_took)
{
	struct ubi_device_info info_before = { 0 };
	struct ubi_device_info info_after = { 0 };
	uint64_t before[UBI_TEST_PEB_COUNT] = { 0 };
	uint64_t after[UBI_TEST_PEB_COUNT] = { 0 };
	uint8_t churned[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint64_t erased = 0;
	uint64_t least = UINT64_MAX;
	uint64_t most = 0;
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_true(0 < pool_ready(&vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	erase_counts_on_flash(config.ikm_key_id, before, ARRAY_SIZE(before));
	flash_ops_forget();

	churn(vol_id, CONFIG_UBI_TEST_LIFETIME_ROUNDS, LEVELLING_ON);

	zassert_ok(ubi_device_get_info(ubi, &info_after));
	erase_counts_on_flash(config.ikm_key_id, after, ARRAY_SIZE(after));

	for (uint32_t pnum = 0; pnum < ARRAY_SIZE(after); ++pnum) {
		const uint64_t gained = after[pnum] - before[pnum];

		zassert_equal(gained, flash_erases_of(pnum), "block %u", pnum);
		erased += gained;
		least = MIN(least, after[pnum]);
		most = MAX(most, after[pnum]);
	}

	zassert_equal(erased,
		      info_after.total_erase_count -
			      info_before.total_erase_count,
		      "the device's total has to add up to the flash's");

	pattern_fill(churned, sizeof(churned), CHURN_SEED);

	const uint32_t churned_pnum =
		pnum_of_data_matching(churned, sizeof(churned));

	zassert_equal(after[churned_pnum], leb_wear(vol_id, 0),
		      "a block's count has to be the one on its header");

	zassert_true(most - least <= CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the flash was worn from %" PRIu64 " to %" PRIu64, least,
		     most);

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_FLASH_SIMULATOR */
