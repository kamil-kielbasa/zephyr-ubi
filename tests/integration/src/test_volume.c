/**
 * \file    test_volume.c
 * \author  Kamil Kielbasa
 * \brief   Creating, finding and removing volumes.
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

/* Module defines ---------------------------------------------------------- */

/** Bytes written into a logical block when a test needs to tell them apart. */
#define LEB_PATTERN_SIZE (64)

/* Static function definitions --------------------------------------------- */

/**
 * \brief Write a pattern the block number decides into a range of blocks.
 */
static void volume_fill(uint32_t vol_id, uint32_t from, uint32_t to,
			uint8_t seed)
{
	uint8_t written[LEB_PATTERN_SIZE];

	for (uint32_t lnum = from; lnum < to; ++lnum) {
		memset(written, (uint8_t)(seed + lnum), sizeof(written));
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));
	}
}

/**
 * \brief Read every block of a volume back and insist on finding it whole.
 */
static void volume_verify(uint32_t vol_id, uint32_t leb_count, uint8_t seed)
{
	uint8_t expected[LEB_PATTERN_SIZE];
	uint8_t read[LEB_PATTERN_SIZE];

	for (uint32_t lnum = 0; lnum < leb_count; ++lnum) {
		memset(expected, (uint8_t)(seed + lnum), sizeof(expected));
		memset(read, 0x00, sizeof(read));

		zassert_ok(
			ubi_leb_read(ubi, vol_id, lnum, 0, read, sizeof(read)));
		zassert_mem_equal(expected, read, sizeof(read),
				  "volume %u block %u lost its contents",
				  vol_id, lnum);
	}
}

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_integration, test_a_created_volume_survives_a_reattach)
{
	const struct ubi_volume_config wanted = { .name = "config",
						  .leb_count = 8 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_find(ubi, "config", &found));
	zassert_equal(vol_id, found);

	zassert_ok(ubi_volume_get_info(ubi, found, &info));
	zassert_equal(8, info.leb_count);
	zassert_equal(0, info.mapped_lebs,
		      "creation reserves, it does not map");
	zassert_equal(0, strcmp("config", info.name));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_creating_a_volume_bumps_the_revision)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_device_info before = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.revision + 1, after.revision);
	zassert_equal(1, after.volume_count);
	zassert_true(after.global_sqnum > before.global_sqnum);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_update_leaves_both_volume_table_copies_current)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	memset(event_seen, 0, sizeof(event_seen));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_false(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "an update has to leave both copies carrying it");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_degraded_volume_table_heals_on_the_next_update)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_equal(1, corrupt_volume_tables(ubi, &config, 1));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	memset(event_seen, 0, sizeof(event_seen));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_false(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "an update writes both copies, so it repairs the pair");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_name_is_refused_when_it_is_already_taken)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-EEXIST, ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_volume_bigger_than_the_partition_is_refused)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "huge" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.peb_count;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = 0;
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id));

	/* Three blocks are held back: two for the volume table and one for an
	 * atomic rewrite to land in. */
	wanted.leb_count = info.peb_count - 3;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_removing_a_volume_frees_its_logical_blocks)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "first" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.peb_count - 3;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	wanted.name = "second";
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_volume_identifiers_are_never_reused)
{
	const struct ubi_volume_config first = { .name = "first",
						 .leb_count = 2 };
	const struct ubi_volume_config second = { .name = "second",
						  .leb_count = 2 };
	uint32_t first_id = UBI_VOL_ID_INVALID;
	uint32_t second_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_create(ubi, &first, &first_id));
	zassert_ok(ubi_volume_remove(ubi, first_id));
	zassert_ok(ubi_volume_create(ubi, &second, &second_id));

	zassert_not_equal(first_id, second_id,
			  "a released identifier must not come back");
	zassert_equal(-ENOENT, ubi_volume_remove(ubi, first_id));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_the_middle_volume_can_be_removed)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };
	struct ubi_volume_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i)
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));

	zassert_ok(ubi_volume_remove(ubi, vol_id[1]));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id[1], &info));

	zassert_ok(ubi_volume_get_info(ubi, vol_id[0], &info));
	zassert_equal(2, info.leb_count);

	zassert_ok(ubi_volume_get_info(ubi, vol_id[2], &info));
	zassert_equal(4, info.leb_count);
	zassert_equal(0, strcmp("c", info.name));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_the_volume_table_is_out_of_the_applications_reach)
{
	const struct ubi_volume_config named = { .name = "volume table",
						 .leb_count = 2 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &named, &vol_id));
	zassert_equal(-ENOENT, ubi_volume_find(ubi, "volume table", &vol_id));
	zassert_equal(-EINVAL, ubi_volume_get_info(ubi, 0xFFFFFFFEUL, &info));
	zassert_equal(-EINVAL, ubi_volume_remove(ubi, 0xFFFFFFFEUL));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_detached_handle_takes_no_volumes)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 2 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_remove(ubi, 0));
	zassert_equal(-EINVAL, ubi_volume_find(ubi, "logs", &vol_id));
	zassert_equal(-EINVAL, ubi_volume_get_info(ubi, 0, &info));
}

ZTEST(ubi_integration, test_reformatting_forgets_the_volumes)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info volume = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_equal(0, device.volume_count,
		      "a format has to leave no volume behind");
	zassert_equal(-ENOENT, ubi_volume_find(ubi, "logs", &vol_id));
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 0, &volume));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_volume_grows_into_what_is_left)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_volume_resize(ubi, vol_id, 16));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(16, info.leb_count);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(16, info.leb_count,
		      "the new size has to be on the "
		      "flash, not only in RAM");
	zassert_equal(0, strcmp("logs", info.name));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_volume_shrinks_and_hands_the_blocks_back)
{
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "logs" };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t other_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &device));

	/* The whole pool, so nothing is left for a second volume. */
	wanted.leb_count = device.peb_count - 3;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.name = "config";
	wanted.leb_count = 4;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &other_id));

	zassert_ok(ubi_volume_resize(ubi, vol_id, device.peb_count - 7));
	zassert_ok(ubi_volume_create(ubi, &wanted, &other_id));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(device.peb_count - 7, info.leb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_volume_cannot_grow_past_what_is_left)
{
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info info = { 0 };
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-ENOSPC,
		      ubi_volume_resize(ubi, vol_id, device.peb_count - 2));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(4, info.leb_count, "a refusal has to change nothing");

	/* The whole pool, to the block. */
	zassert_ok(ubi_volume_resize(ubi, vol_id, device.peb_count - 3));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_volume_cannot_be_resized_to_nothing)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, 0));
	zassert_equal(-ENOENT, ubi_volume_resize(ubi, vol_id + 1, 2));
	zassert_equal(-EINVAL, ubi_volume_resize(ubi, 0xFFFFFFFEUL, 2));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(4, info.leb_count);

	zassert_ok(ubi_volume_resize(ubi, vol_id, 1));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_resizing_the_middle_volume_keeps_the_others)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };
	struct ubi_volume_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i)
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], 9));
	zassert_ok(ubi_volume_resize(ubi, vol_id[1], 1));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_get_info(ubi, vol_id[0], &info));
	zassert_equal(2, info.leb_count);
	zassert_equal(0, strcmp("a", info.name));

	zassert_ok(ubi_volume_get_info(ubi, vol_id[1], &info));
	zassert_equal(1, info.leb_count);

	zassert_ok(ubi_volume_get_info(ubi, vol_id[2], &info));
	zassert_equal(4, info.leb_count);
	zassert_equal(0, strcmp("c", info.name));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_resizing_to_the_same_size_writes_nothing)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_volume_resize(ubi, vol_id, 4));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.revision, after.revision,
		      "a size that did not change is not an update");
	zassert_equal(before.global_sqnum, after.global_sqnum);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_the_logical_budget_follows_the_volumes)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 4 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	const uint32_t budget = info.free_lebs;

	zassert_equal(info.peb_count - 3, budget,
		      "two blocks hold the volume table and one is kept for "
		      "an atomic rewrite");
	zassert_equal(0, info.relocatable_pebs,
		      "a freshly formatted device is evenly worn");

	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - 4, info.free_lebs);

	zassert_ok(ubi_volume_resize(ubi, vol_id, 10));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - 10, info.free_lebs);

	/* What it reports has to be exactly what the next call may ask for. */
	wanted.name = "rest";
	wanted.leb_count = info.free_lebs + 1;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_lebs);

	zassert_ok(ubi_volume_remove(ubi, vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - 10, info.free_lebs, "removing gives it back");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_removing_a_volume_keeps_the_others_mappings)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));
		volume_fill(vol_id[i], 0, wanted[i].leb_count,
			    (uint8_t)(0xA0 + 0x10 * i));
	}

	zassert_ok(ubi_volume_remove(ubi, vol_id[1]));

	/* The survivors' slices of the mapping table moved; every block they
	 * hold has to have moved with them. */
	volume_verify(vol_id[0], wanted[0].leb_count, 0xA0);
	volume_verify(vol_id[2], wanted[2].leb_count, 0xC0);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	volume_verify(vol_id[0], wanted[0].leb_count, 0xA0);
	volume_verify(vol_id[2], wanted[2].leb_count, 0xC0);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_resizing_a_volume_keeps_the_others_mappings)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));
		volume_fill(vol_id[i], 0, wanted[i].leb_count,
			    (uint8_t)(0xA0 + 0x10 * i));
	}

	/* Growing the middle one pushes both neighbours' slices along. */
	zassert_ok(ubi_volume_resize(ubi, vol_id[1], 6));

	/* The blocks that appear must be the volume's own and empty, not
	 * whatever the neighbour's mappings left in those slots. */
	for (uint32_t lnum = wanted[1].leb_count; lnum < 6; ++lnum) {
		struct ubi_leb_info info = { 0 };

		zassert_ok(ubi_leb_get_info(ubi, vol_id[1], lnum, &info));
		zassert_false(info.mapped,
			      "block %u came with somebody else's mapping",
			      lnum);
	}

	volume_fill(vol_id[1], wanted[1].leb_count, 6, 0xB0);

	volume_verify(vol_id[0], wanted[0].leb_count, 0xA0);
	volume_verify(vol_id[1], 6, 0xB0);
	volume_verify(vol_id[2], wanted[2].leb_count, 0xC0);

	/* Shrinking pulls them back, and what stays has to still read. */
	for (uint32_t lnum = 2; lnum < 6; ++lnum)
		zassert_ok(ubi_leb_unmap(ubi, vol_id[1], lnum));

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], 2));

	volume_verify(vol_id[0], wanted[0].leb_count, 0xA0);
	volume_verify(vol_id[1], 2, 0xB0);
	volume_verify(vol_id[2], wanted[2].leb_count, 0xC0);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	volume_verify(vol_id[0], wanted[0].leb_count, 0xA0);
	volume_verify(vol_id[1], 2, 0xB0);
	volume_verify(vol_id[2], wanted[2].leb_count, 0xC0);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_shrinking_over_a_mapped_block_is_refused)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[64];

	memset(written, 0xC7, sizeof(written));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_leb_change(ubi, vol_id, 3, written, sizeof(written)));

	zassert_equal(-EBUSY, ubi_volume_resize(ubi, vol_id, 2),
		      "the tail still carries data");

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(4, info.leb_count, "a refusal has to change nothing");

	/* Say so explicitly and the tail may go. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 3));
	zassert_ok(ubi_volume_resize(ubi, vol_id, 2));

	zassert_ok(ubi_device_deinit(ubi));
}
