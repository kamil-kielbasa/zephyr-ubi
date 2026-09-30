/**
 * \file    test_volume_resize.c
 * \author  Kamil Kielbasa
 * \brief   Growing and shrinking volumes, and the logical blocks they share.
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
#include "partition.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Blocks no volume may have: the volume table copies, and one for an
 *  atomic rewrite to land in. */
#define RESERVED_PEBS (UBI_VOLUME_TABLE_LEB_COUNT + 1)

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_volume_resize);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a freshly formatted device.
 * When:  volumes are created, resized and removed.
 * Then:  the free logical block count follows every step, what it reports
 *        is exactly what the next call may ask for, and a size of zero is
 *        refused as a bad argument.
 */
ZTEST(ubi_volume_resize, test_the_logical_budget_follows_the_volumes)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "logs",
					    .leb_count = UBI_TEST_VOLUME_LEBS };
	const uint32_t grown = 2 * UBI_TEST_VOLUME_LEBS;
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	const uint32_t budget = info.free_lebs;

	zassert_equal(info.peb_count - RESERVED_PEBS, budget);
	zassert_equal(0, info.relocatable_pebs,
		      "a freshly formatted device is evenly worn");

	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - wanted.leb_count, info.free_lebs);

	zassert_ok(ubi_volume_resize(ubi, vol_id, grown));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - grown, info.free_lebs);

	wanted.name = "rest";
	wanted.leb_count = 0;
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = info.free_lebs + 1;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_lebs);

	zassert_ok(ubi_volume_remove(ubi, vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - grown, info.free_lebs, "removing gives it back");
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id),
		   "and the same size can be asked for again");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a small volume.
 * When:  it is grown and the device attached again.
 * Then:  the new size is on the flash rather than only in RAM.
 */
ZTEST(ubi_volume_resize, test_a_volume_grows_into_what_is_left)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const uint32_t grown = 4 * UBI_TEST_VOLUME_LEBS;
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_volume_resize(ubi, vol_id, grown));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(grown, info.leb_count);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(grown, info.leb_count,
		      "the new size has to be on the flash, not only in RAM");
	zassert_str_equal(wanted.name, info.name);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: one volume holding the whole logical budget, leaving no room.
 * When:  it is shrunk.
 * Then:  the blocks it let go become available to a second volume.
 */
ZTEST(ubi_volume_resize, test_a_volume_shrinks_and_hands_the_blocks_back)
{
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info info = { 0 };
	struct ubi_volume_config whole = { .name = "logs", .leb_count = 0 };
	const struct ubi_volume_config other = {
		.name = "config", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t other_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &device));

	whole.leb_count = device.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &whole, &vol_id));
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &other, &other_id));

	const uint32_t shrunk = whole.leb_count - other.leb_count;

	zassert_ok(ubi_volume_resize(ubi, vol_id, shrunk));
	zassert_ok(ubi_volume_create(ubi, &other, &other_id));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(shrunk, info.leb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a small volume.
 * When:  it is asked to grow past what the partition has left.
 * Then:  the refusal changes nothing, and growing to exactly the whole
 *        budget still goes through.
 */
ZTEST(ubi_volume_resize, test_a_volume_cannot_grow_past_what_is_left)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-ENOSPC,
		      ubi_volume_resize(ubi, vol_id, device.free_lebs + 1));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(wanted.leb_count, info.leb_count,
		      "a refusal has to change nothing");

	zassert_ok(ubi_volume_resize(ubi, vol_id, device.free_lebs));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a small volume.
 * When:  it is resized to nothing, or a volume that is not the caller's is
 *        named.
 * Then:  each is refused without changing the volume, and one block is a
 *        size the volume may legitimately take.
 */
ZTEST(ubi_volume_resize, test_a_volume_cannot_be_resized_to_nothing)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, 0));
	zassert_equal(-ENOENT, ubi_volume_resize(ubi, vol_id + 1, 1));
	zassert_equal(-EINVAL,
		      ubi_volume_resize(ubi, UBI_VOLUME_TABLE_VOL_ID, 1));

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(wanted.leb_count, info.leb_count);

	zassert_ok(ubi_volume_resize(ubi, vol_id, 1));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a small volume.
 * When:  it is resized to the size it already has.
 * Then:  nothing is written, because a size that did not change is not an
 *        update to pay for.
 */
ZTEST(ubi_volume_resize, test_resizing_to_the_same_size_writes_nothing)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_volume_resize(ubi, vol_id, wanted.leb_count));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.revision, after.revision,
		      "a size that did not change is not an update");
	zassert_equal(before.max_sqnum, after.max_sqnum);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: three volumes, each with every logical block written.
 * When:  the middle one is grown and then shrunk again, and the device
 *        attached again.
 * Then:  the blocks that appear are the volume's own and empty, and every
 *        volume reports its name and size and holds every byte it should.
 */
ZTEST(ubi_volume_resize, test_resizing_a_volume_keeps_the_others)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	const uint8_t seed[ARRAY_SIZE(wanted)] = { 0xA0, 0xB0, 0xC0 };
	const uint32_t grown = 2 * wanted[1].leb_count;
	const uint32_t shrunk = wanted[1].leb_count - 1;
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };
	struct ubi_volume_info volume = { 0 };
	struct ubi_leb_info leb = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));
		volume_fill(vol_id[i], 0, wanted[i].leb_count, seed[i]);
	}

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], grown));

	for (uint32_t lnum = wanted[1].leb_count; lnum < grown; ++lnum) {
		zassert_ok(ubi_leb_get_info(ubi, vol_id[1], lnum, &leb));
		zassert_false(leb.mapped,
			      "block %u came with somebody else's mapping",
			      lnum);
	}

	volume_fill(vol_id[1], wanted[1].leb_count, grown, seed[1]);

	volume_check(vol_id[0], 0, wanted[0].leb_count, seed[0]);
	volume_check(vol_id[1], 0, grown, seed[1]);
	volume_check(vol_id[2], 0, wanted[2].leb_count, seed[2]);

	for (uint32_t lnum = shrunk; lnum < grown; ++lnum)
		zassert_ok(ubi_leb_unmap(ubi, vol_id[1], lnum));

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], shrunk));

	volume_check(vol_id[0], 0, wanted[0].leb_count, seed[0]);
	volume_check(vol_id[1], 0, shrunk, seed[1]);
	volume_check(vol_id[2], 0, wanted[2].leb_count, seed[2]);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		const uint32_t size = (1 == i) ? shrunk : wanted[i].leb_count;

		zassert_ok(ubi_volume_get_info(ubi, vol_id[i], &volume));
		zassert_equal(size, volume.leb_count);
		zassert_str_equal(wanted[i].name, volume.name);
		volume_check(vol_id[i], 0, size, seed[i]);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume whose last block carries data.
 * When:  it is shrunk over that block.
 * Then:  the refusal changes nothing, and saying so explicitly by unmapping
 *        the block lets the resize through.
 */
ZTEST(ubi_volume_resize, test_shrinking_over_a_mapped_block_is_refused)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const uint32_t last = wanted.leb_count - 1;
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xC7);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_leb_change(ubi, vol_id, last, written, sizeof(written)));

	zassert_equal(-EBUSY, ubi_volume_resize(ubi, vol_id, last),
		      "the last block still carries data");

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &info));
	zassert_equal(wanted.leb_count, info.leb_count,
		      "a refusal has to change nothing");

	zassert_ok(ubi_leb_unmap(ubi, vol_id, last));
	zassert_ok(ubi_volume_resize(ubi, vol_id, last));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block unmapped and shrunk away, its old copy still waiting for
 *        reclaim.
 * When:  the volume grows back over it and the device is attached again.
 * Then:  the block comes back unmapped and its old contents are gone: what
 *        a volume gave up must not return to it.
 */
ZTEST(ubi_volume_resize,
      test_growing_back_does_not_bring_back_what_was_shrunk_away)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const uint32_t last = UBI_TEST_VOLUME_LEBS - 1;
	struct ubi_leb_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xD8);

	zassert_ok(ubi_leb_change(ubi, vol_id, last, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, last));
	zassert_ok(ubi_volume_resize(ubi, vol_id, last));
	zassert_ok(ubi_volume_resize(ubi, vol_id, UBI_TEST_VOLUME_LEBS));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, last, &info));
	zassert_false(info.mapped, "a block grown back has to start empty");
	zassert_equal(0, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume filled, its tail unmapped and the volume shrunk, which
 *        cleared the table but not the flash.
 * When:  the device is attached again.
 * Then:  the blocks past the new size are taken back quietly while the one
 *        still within it reads what it held.
 */
ZTEST(ubi_volume_resize,
      test_blocks_past_a_shrunk_volume_are_taken_back_quietly)
{
	const struct ubi_volume_config wanted = {
		.name = "shrunk", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const uint32_t shrunk = 1;
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	volume_fill(vol_id, 0, wanted.leb_count, 0x50);

	for (uint32_t lnum = shrunk; lnum < wanted.leb_count; ++lnum)
		zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

	zassert_ok(ubi_volume_resize(ubi, vol_id, shrunk));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(0, events_total,
		      "the application shrank the volume itself");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.reclaimable_pebs, after.reclaimable_pebs,
		      "what the shrink let go still waits for reclaim");

	volume_check(vol_id, 0, shrunk, 0x50);

	zassert_ok(ubi_device_deinit(ubi));
}
