/**
 * \file    test_volume.c
 * \author  Kamil Kielbasa
 * \brief   Creating, finding and removing volumes, and the volume table
 *          updates they cost.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "forge.h"
#include "partition.h"
#include "suite.h"
#include "table_copies.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_volume);

/* Module interface function definitions ----------------------------------- */

/* Tests: creating and finding --------------------------------------------- */

/*
 * Given: a formatted device with one volume created on it.
 * When:  it is detached and attached again.
 * Then:  the volume is found by name with the properties it was given, and
 *        creation reserved blocks without mapping any.
 */
ZTEST(ubi_volume, test_a_created_volume_survives_a_reattach)
{
	const struct ubi_volume_config wanted = {
		.name = "config", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_find(ubi, wanted.name, &found));
	zassert_equal(vol_id, found);

	zassert_ok(ubi_volume_get_info(ubi, found, &info));
	zassert_equal(wanted.leb_count, info.leb_count);
	zassert_equal(0, info.mapped_lebs,
		      "creation reserves, it does not map");
	zassert_str_equal(wanted.name, info.name);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a freshly formatted device.
 * When:  a volume is created.
 * Then:  the volume table moves to a new revision and spends a sequence
 *        number on each copy, so an older copy can never outrank it.
 */
ZTEST(ubi_volume, test_creating_a_volume_bumps_the_revision)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.revision + 1, after.revision);
	zassert_equal(1, after.volume_count);
	zassert_equal(before.max_sqnum + UBI_VOLUME_TABLE_LEB_COUNT,
		      after.max_sqnum, "one sequence number per copy");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a freshly formatted device.
 * When:  a volume is created, which updates the volume table.
 * Then:  each copy moved to a block of its own at the new revision, no other
 *        block on the flash still claims a copy, and the next attach finds
 *        the pair whole.
 */
ZTEST(ubi_volume, test_an_update_moves_both_copies_to_fresh_blocks)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t before[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t after[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT + 1] = { 0 };
	uint32_t revisions[UBI_VOLUME_TABLE_LEB_COUNT + 1] = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	volume_table_copy_blocks(config.ikm_key_id, before);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copy_blocks(config.ikm_key_id, after);

	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum) {
		zassert_not_equal(before[lnum], after[lnum],
				  "copy %u was rewritten in place", lnum);
	}

	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT,
		      volume_table_adoptable(config.ikm_key_id, pnums,
					     revisions, ARRAY_SIZE(pnums)));

	for (uint32_t i = 0; i < UBI_VOLUME_TABLE_LEB_COUNT; ++i) {
		zassert_equal(info.revision, revisions[i],
			      "the copy in PEB %u is stale", pnums[i]);
	}

	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT,
		      volume_table_blocks(config.ikm_key_id, pnums,
					  ARRAY_SIZE(pnums)),
		      "the blocks the copies left have to be erased");

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(0, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED]);
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device attached with one volume table copy damaged.
 * When:  a volume is created, which rewrites the table.
 * Then:  the update writes both copies, so the pair is whole again without
 *        anyone asking for a repair.
 */
ZTEST(ubi_volume, test_a_degraded_volume_table_heals_on_the_next_update)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_equal(1, corrupt_volume_tables(config.ikm_key_id, 1));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(0, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "an update writes both copies, so it repairs the pair");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device that already holds a volume called "logs".
 * When:  another volume asks for the same name.
 * Then:  it is refused and the device still holds exactly one volume.
 */
ZTEST(ubi_volume, test_a_name_is_refused_when_it_is_already_taken)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
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

/*
 * Given: a volume that has been created and removed.
 * When:  another volume is created.
 * Then:  it is given a fresh identifier and the old one stays gone, so a
 *        stale reference can never reach somebody else's data.
 */
ZTEST(ubi_volume, test_volume_identifiers_are_never_reused)
{
	const struct ubi_volume_config first = {
		.name = "first", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
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

/*
 * Given: an attached device.
 * When:  the application reaches for the volume holding the volume table,
 *        by name and by identifier.
 * Then:  every route to it is refused, because it is not the application's.
 */
ZTEST(ubi_volume, test_the_volume_table_is_out_of_the_applications_reach)
{
	const struct ubi_volume_config named = {
		.name = UBI_VOLUME_TABLE_NAME, .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &named, &vol_id));
	zassert_equal(-ENOENT,
		      ubi_volume_find(ubi, UBI_VOLUME_TABLE_NAME, &vol_id));
	zassert_equal(-EINVAL,
		      ubi_volume_get_info(ubi, UBI_VOLUME_TABLE_VOL_ID, &info));
	zassert_equal(-EINVAL, ubi_volume_remove(ubi, UBI_VOLUME_TABLE_VOL_ID));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device holding a volume.
 * When:  the partition is formatted again.
 * Then:  no volume is left behind, by count, by name or by identifier.
 */
ZTEST(ubi_volume, test_reformatting_forgets_the_volumes)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info volume = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_equal(0, device.volume_count,
		      "a format has to leave no volume behind");
	zassert_equal(-ENOENT, ubi_volume_find(ubi, wanted.name, &found));
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id, &volume));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: removing --------------------------------------------------------- */

/*
 * Given: three volumes, each with every logical block written.
 * When:  the middle one is removed, and the device attached again.
 * Then:  it is gone, and its neighbours kept their names, their sizes and
 *        every byte they held.
 */
ZTEST(ubi_volume, test_removing_a_volume_keeps_the_others)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	const uint8_t seed[ARRAY_SIZE(wanted)] = { 0xA0, 0xB0, 0xC0 };
	const size_t kept[] = { 0, 2 };
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };
	struct ubi_volume_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));
		volume_fill(vol_id[i], 0, wanted[i].leb_count, seed[i]);
	}

	zassert_ok(ubi_volume_remove(ubi, vol_id[1]));

	for (size_t k = 0; k < ARRAY_SIZE(kept); ++k) {
		const size_t i = kept[k];

		volume_check(vol_id[i], 0, wanted[i].leb_count, seed[i]);
	}

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id[1], &info));

	for (size_t k = 0; k < ARRAY_SIZE(kept); ++k) {
		const size_t i = kept[k];

		zassert_ok(ubi_volume_get_info(ubi, vol_id[i], &info));
		zassert_equal(wanted[i].leb_count, info.leb_count);
		zassert_str_equal(wanted[i].name, info.name);
		volume_check(vol_id[i], 0, wanted[i].leb_count, seed[i]);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume that was filled and then removed, which only queued its
 *        blocks rather than erasing them.
 * When:  the device is attached again.
 * Then:  the blocks still naming the gone volume are taken back quietly, and
 *        a reclaim run clears them for good.
 */
ZTEST(ubi_volume, test_blocks_of_a_removed_volume_are_taken_back_quietly)
{
	const struct ubi_volume_config wanted = {
		.name = "gone", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	volume_fill(vol_id, 0, wanted.leb_count, 0x40);
	zassert_ok(ubi_volume_remove(ubi, vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(0, events_total,
		      "the application removed the volume itself");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);

	/* Released blocks are reclaimed before blank ones. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   wanted.leb_count, &result));
	zassert_equal(wanted.leb_count, result.performed);
	zassert_ok(ubi_device_deinit(ubi));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum) {
		leb_payload(0x40, lnum, written, sizeof(written));
		zassert_equal(0, count_data_matching(written, sizeof(written)),
			      "block %u has to be gone after the reclaim",
			      lnum);
	}
}

/*
 * Given: a block whose authentic header names a volume identifier this
 *        device never handed out.
 * When:  the device is attached.
 * Then:  that block alone is reported as orphaned and queued for reclaim,
 *        because nothing this device did explains it.
 */
ZTEST(ubi_volume, test_a_block_of_a_volume_never_created_is_orphaned)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const uint32_t stranger = vol_id + 7;
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x61);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	vid_rewrite(config.ikm_key_id,
		    pnum_of_data_matching(written, sizeof(written)), stranger,
		    0);

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(1, event_count[UBI_EVENT_LEB_ORPHANED]);
	zassert_equal(stranger, event_last[UBI_EVENT_LEB_ORPHANED].vol_id);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.reclaimable_pebs + 1, after.reclaimable_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}
