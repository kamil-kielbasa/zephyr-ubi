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
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Blocks no volume may have: the volume table copies, and one for an
 *  atomic rewrite to land in. */
#define RESERVED_PEBS (UBI_VOLUME_TABLE_LEB_COUNT + 1)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Write leb_payload() of \p seed into blocks \p from to \p to - 1.
 */
static void volume_fill(uint32_t vol_id, uint32_t from, uint32_t to,
			uint8_t seed);

/**
 * \brief Fail unless every block below \p leb_count holds what
 *        volume_fill() wrote from \p seed.
 */
static void volume_check(uint32_t vol_id, uint32_t leb_count, uint8_t seed);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_volume);

/* Static function definitions --------------------------------------------- */

static void volume_fill(uint32_t vol_id, uint32_t from, uint32_t to,
			uint8_t seed)
{
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	for (uint32_t lnum = from; lnum < to; ++lnum) {
		leb_payload(seed, lnum, written, sizeof(written));
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));
	}
}

static void volume_check(uint32_t vol_id, uint32_t leb_count, uint8_t seed)
{
	uint8_t expected[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	for (uint32_t lnum = 0; lnum < leb_count; ++lnum) {
		leb_payload(seed, lnum, expected, sizeof(expected));
		memset(read, 0x00, sizeof(read));

		zassert_ok(
			ubi_leb_read(ubi, vol_id, lnum, 0, read, sizeof(read)));
		zassert_mem_equal(expected, read, sizeof(read),
				  "volume %u block %u lost its contents",
				  vol_id, lnum);
	}
}

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
	zassert_equal(before.global_sqnum + UBI_VOLUME_TABLE_LEB_COUNT,
		      after.global_sqnum, "one sequence number per copy");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose volume table was just updated.
 * When:  it is attached again.
 * Then:  neither copy is stale, so an update has to leave both carrying it.
 */
ZTEST(ubi_volume, test_an_update_leaves_both_volume_table_copies_current)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(0, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "an update has to leave both copies carrying it");

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
 * Given: a freshly formatted device.
 * When:  volumes of impossible sizes are asked for.
 * Then:  too large is refused for want of room and zero as a bad argument,
 *        while everything the partition can share out is granted.
 */
ZTEST(ubi_volume, test_a_volume_bigger_than_the_partition_is_refused)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "huge", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.peb_count;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = 0;
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume holding every logical block the partition can share out.
 * When:  it is removed and another of the same size asked for.
 * Then:  the second one is granted, so removal really hands the budget back.
 */
ZTEST(ubi_volume, test_removing_a_volume_frees_its_logical_blocks)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_config wanted = { .name = "first", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	wanted.name = "second";
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

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
 * Given: three volumes, of which the middle one is removed.
 * When:  the device is attached again.
 * Then:  the middle one is gone and its neighbours kept their sizes and
 *        names, so the table survived a record being taken out of it.
 */
ZTEST(ubi_volume, test_the_middle_volume_can_be_removed)
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
	zassert_equal(wanted[0].leb_count, info.leb_count);
	zassert_str_equal(wanted[0].name, info.name);

	zassert_ok(ubi_volume_get_info(ubi, vol_id[2], &info));
	zassert_equal(wanted[2].leb_count, info.leb_count);
	zassert_str_equal(wanted[2].name, info.name);

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
 * Given: a formatted partition and a handle nobody attached.
 * When:  any volume call is made on it.
 * Then:  each one is refused rather than reaching the flash.
 */
ZTEST(ubi_volume, test_a_detached_handle_takes_no_volumes)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_volume_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_remove(ubi, 0));
	zassert_equal(-EINVAL, ubi_volume_find(ubi, wanted.name, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_get_info(ubi, 0, &info));
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

/* Tests: resizing --------------------------------------------------------- */

/*
 * Given: a small volume.
 * When:  it is grown and the device attached again.
 * Then:  the new size is on the flash rather than only in RAM.
 */
ZTEST(ubi_volume, test_a_volume_grows_into_what_is_left)
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
ZTEST(ubi_volume, test_a_volume_shrinks_and_hands_the_blocks_back)
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
ZTEST(ubi_volume, test_a_volume_cannot_grow_past_what_is_left)
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
ZTEST(ubi_volume, test_a_volume_cannot_be_resized_to_nothing)
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
 * Given: three volumes, with the middle one grown and then shrunk.
 * When:  the device is attached again.
 * Then:  all three report the sizes and names they should, so moving a
 *        record about did not disturb its neighbours.
 */
ZTEST(ubi_volume, test_resizing_the_middle_volume_keeps_the_others)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	const uint32_t grown = 3 * wanted[1].leb_count;
	const uint32_t shrunk = 1;
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };
	struct ubi_volume_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i)
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], grown));
	zassert_ok(ubi_volume_resize(ubi, vol_id[1], shrunk));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_get_info(ubi, vol_id[0], &info));
	zassert_equal(wanted[0].leb_count, info.leb_count);
	zassert_str_equal(wanted[0].name, info.name);

	zassert_ok(ubi_volume_get_info(ubi, vol_id[1], &info));
	zassert_equal(shrunk, info.leb_count);

	zassert_ok(ubi_volume_get_info(ubi, vol_id[2], &info));
	zassert_equal(wanted[2].leb_count, info.leb_count);
	zassert_str_equal(wanted[2].name, info.name);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a small volume.
 * When:  it is resized to the size it already has.
 * Then:  nothing is written, because a size that did not change is not an
 *        update to pay for.
 */
ZTEST(ubi_volume, test_resizing_to_the_same_size_writes_nothing)
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
	zassert_equal(before.global_sqnum, after.global_sqnum);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a freshly formatted device.
 * When:  volumes are created, resized and removed.
 * Then:  the free logical block count follows every step, and what it
 *        reports is exactly what the next call may ask for.
 */
ZTEST(ubi_volume, test_the_logical_budget_follows_the_volumes)
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
	wanted.leb_count = info.free_lebs + 1;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_lebs);

	zassert_ok(ubi_volume_remove(ubi, vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(budget - grown, info.free_lebs, "removing gives it back");

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what the volumes hold -------------------------------------------- */

/*
 * Given: three volumes, each with every logical block written.
 * When:  the middle one is removed.
 * Then:  the survivors' slices of the mapping table moved with them, so
 *        every block they hold still reads what it held.
 */
ZTEST(ubi_volume, test_removing_a_volume_keeps_the_others_mappings)
{
	const struct ubi_volume_config wanted[] = {
		{ .name = "a", .leb_count = 2 },
		{ .name = "b", .leb_count = 3 },
		{ .name = "c", .leb_count = 4 },
	};
	const uint8_t seed[ARRAY_SIZE(wanted)] = { 0xA0, 0xB0, 0xC0 };
	uint32_t vol_id[ARRAY_SIZE(wanted)] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));
		volume_fill(vol_id[i], 0, wanted[i].leb_count, seed[i]);
	}

	zassert_ok(ubi_volume_remove(ubi, vol_id[1]));

	volume_check(vol_id[0], wanted[0].leb_count, seed[0]);
	volume_check(vol_id[2], wanted[2].leb_count, seed[2]);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	volume_check(vol_id[0], wanted[0].leb_count, seed[0]);
	volume_check(vol_id[2], wanted[2].leb_count, seed[2]);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: three volumes, each with every logical block written.
 * When:  the middle one is grown and then shrunk again.
 * Then:  the blocks that appear are the volume's own and empty, and no
 *        neighbour lost a byte to the slices moving about.
 */
ZTEST(ubi_volume, test_resizing_a_volume_keeps_the_others_mappings)
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
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (size_t i = 0; i < ARRAY_SIZE(wanted); ++i) {
		zassert_ok(ubi_volume_create(ubi, &wanted[i], &vol_id[i]));
		volume_fill(vol_id[i], 0, wanted[i].leb_count, seed[i]);
	}

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], grown));

	for (uint32_t lnum = wanted[1].leb_count; lnum < grown; ++lnum) {
		zassert_ok(ubi_leb_get_info(ubi, vol_id[1], lnum, &info));
		zassert_false(info.mapped,
			      "block %u came with somebody else's mapping",
			      lnum);
	}

	volume_fill(vol_id[1], wanted[1].leb_count, grown, seed[1]);

	volume_check(vol_id[0], wanted[0].leb_count, seed[0]);
	volume_check(vol_id[1], grown, seed[1]);
	volume_check(vol_id[2], wanted[2].leb_count, seed[2]);

	for (uint32_t lnum = shrunk; lnum < grown; ++lnum)
		zassert_ok(ubi_leb_unmap(ubi, vol_id[1], lnum));

	zassert_ok(ubi_volume_resize(ubi, vol_id[1], shrunk));

	volume_check(vol_id[0], wanted[0].leb_count, seed[0]);
	volume_check(vol_id[1], shrunk, seed[1]);
	volume_check(vol_id[2], wanted[2].leb_count, seed[2]);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	volume_check(vol_id[0], wanted[0].leb_count, seed[0]);
	volume_check(vol_id[1], shrunk, seed[1]);
	volume_check(vol_id[2], wanted[2].leb_count, seed[2]);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume whose last block carries data.
 * When:  it is shrunk over that block.
 * Then:  the refusal changes nothing, and saying so explicitly by unmapping
 *        the block lets the resize through.
 */
ZTEST(ubi_volume, test_shrinking_over_a_mapped_block_is_refused)
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
 * Given: a volume that was filled and then removed, which only queued its
 *        blocks rather than erasing them.
 * When:  the device is attached again.
 * Then:  every block still naming the gone volume is reported as orphaned,
 *        and a reclaim run clears the claim for good.
 */
ZTEST(ubi_volume, test_blocks_of_a_removed_volume_are_reported_as_orphaned)
{
	const struct ubi_volume_config wanted = {
		.name = "gone", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	volume_fill(vol_id, 0, wanted.leb_count, 0x40);
	zassert_ok(ubi_volume_remove(ubi, vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(wanted.leb_count, event_count[UBI_EVENT_LEB_ORPHANED]);
	zassert_equal(vol_id, event_last[UBI_EVENT_LEB_ORPHANED].vol_id);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);

	/* Released blocks are reclaimed before blank ones. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   wanted.leb_count, &result));
	zassert_equal(wanted.leb_count, result.performed);
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(0, event_count[UBI_EVENT_LEB_ORPHANED],
		      "reclaim has to clear the claim for good");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume filled and then shrunk, which cleared the table but not
 *        the flash.
 * When:  the device is attached again.
 * Then:  the blocks past the new size are reported as orphaned while the
 *        one still within it reads what it held.
 */
ZTEST(ubi_volume, test_blocks_past_a_shrunk_volume_are_reported_as_orphaned)
{
	const struct ubi_volume_config wanted = {
		.name = "shrunk", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const uint32_t shrunk = 1;
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	volume_fill(vol_id, 0, wanted.leb_count, 0x50);

	/* Unmapping clears the table, not the flash. */
	zassert_equal(-EBUSY, ubi_volume_resize(ubi, vol_id, shrunk));

	for (uint32_t lnum = shrunk; lnum < wanted.leb_count; ++lnum)
		zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

	zassert_ok(ubi_volume_resize(ubi, vol_id, shrunk));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(wanted.leb_count - shrunk,
		      event_count[UBI_EVENT_LEB_ORPHANED]);
	zassert_equal(vol_id, event_last[UBI_EVENT_LEB_ORPHANED].vol_id);
	zassert_not_equal(0, event_last[UBI_EVENT_LEB_ORPHANED].lnum,
			  "block 0 is still within the volume");

	volume_check(vol_id, shrunk, 0x50);

	zassert_ok(ubi_device_deinit(ubi));
}
