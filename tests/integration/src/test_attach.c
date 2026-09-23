/**
 * \file    test_attach.c
 * \author  Kamil Kielbasa
 * \brief   Recognising what is on the flash, and leaving it alone.
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

/** First block a fresh format leaves blank, behind the volume table copies. */
#define FIRST_BLANK_PNUM (UBI_VOLUME_TABLE_LEB_COUNT)

/** Erase count planted in a block left by an earlier image. */
#define EARLIER_ERASE_COUNT (5)

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_attach);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a partition nobody has ever formatted.
 * When:  a device is attached to it.
 * Then:  attach reports there is no UBI device here, rather than making one.
 */
ZTEST(ubi_attach, test_blank_partition_is_not_a_ubi_device)
{
	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

/*
 * Given: a partition full of bytes that are not a UBI image.
 * When:  a device is attached to it.
 * Then:  attach refuses, because those bytes might be somebody's data.
 */
ZTEST(ubi_attach, test_foreign_content_is_not_a_ubi_device)
{
	partition_fill(0x5A);

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

/*
 * Given: a freshly formatted partition.
 * When:  it is attached and asked what it looks like.
 * Then:  the geometry it reports is the flash driver's own, and a clean
 *        attach has nothing to report.
 */
ZTEST(ubi_attach, test_format_then_attach)
{
	struct ubi_device_info info = { 0 };
	const struct flash_area *area = NULL;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &area));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(area->fa_size / UBI_TEST_PEB_SIZE, info.peb_count,
		      "the partition has to be divided by the erase block");
	zassert_equal(UBI_TEST_PEB_SIZE, info.peb_size);
	zassert_equal(UBI_TEST_PEB_SIZE - UBI_DATA_OFFSET, info.leb_size,
		      "two headers precede the data");
	zassert_equal(flash_area_align(area), info.write_block_size,
		      "the geometry has to come from the driver");
	zassert_equal(UBI_TEST_WRITE_BLOCK, info.write_block_size);
	zassert_equal(0, info.volume_count);
	zassert_not_equal(0, info.image_seq);
	zassert_equal(1, info.revision);
	zassert_equal(0, events_total, "a clean attach has nothing to report");

	flash_area_close(area);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a formatted partition that has been attached once already.
 * When:  it is detached and attached again, which is what a reboot looks like.
 * Then:  every counter that anchors the device's identity comes back the same.
 */
ZTEST(ubi_attach, test_attach_survives_a_reboot)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(first.image_seq, second.image_seq);
	zassert_equal(first.revision, second.revision);
	zassert_equal(first.global_sqnum, second.global_sqnum);
	zassert_equal(first.total_erase_count, second.total_erase_count);
	zassert_equal(first.healthy_pebs, second.healthy_pebs);
}

/*
 * Given: a partition that has already carried one formatted image.
 * When:  it is formatted a second time.
 * Then:  the new image takes an identifier of its own, and the counters a
 *        rollback would have to forge carry on rather than restarting: an
 *        old volume table must never be able to outrank the fresh one.
 */
ZTEST(ubi_attach, test_reformatting_starts_a_new_image_without_losing_the_past)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_not_equal(first.image_seq, second.image_seq,
			  "a reformat must not reuse the image sequence");

	zassert_true(
		second.global_sqnum > first.global_sqnum,
		"a reformat must outrank whatever is already on the flash");

	zassert_true(second.total_erase_count > first.total_erase_count,
		     "the wear history has to be carried on");
}

/*
 * Given: one block stamped with an erase count no further erase could raise.
 * When:  the device is attached.
 * Then:  that block alone is retired and reported, and the device still opens.
 */
ZTEST(ubi_attach, test_an_exhausted_erase_counter_retires_the_block)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	stamp_erase_count(config.ikm_key_id, FIRST_BLANK_PNUM, before.image_seq,
			  (uint64_t)UBI_MAX_ERASE_COUNT + 1);
	events_forget();

	zassert_ok(ubi_device_init(ubi, &config),
		   "one finished block must not cost the device");
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(
		before.bad_pebs + 1, after.bad_pebs,
		"a counter that cannot be raised leaves the block unusable");
	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);
	zassert_equal(FIRST_BLANK_PNUM, event_last[UBI_EVENT_PEB_BAD].pnum);
	zassert_equal(before.revision, after.revision);
}

/*
 * Given: a formatted partition and a key that will not verify against it.
 * When:  an attach is attempted with the wrong key.
 * Then:  it is refused without a byte changing, and the right key still opens
 *        the device afterwards.
 */
ZTEST(ubi_attach, test_a_wrong_key_is_refused_without_damage)
{
	uint32_t before = 0;
	uint32_t after = 0;

	zassert_ok(ubi_device_format(&config));

	before = partition_fingerprint();
	zassert_equal(-EBADMSG, ubi_device_init(ubi, &config_wrong_key));
	after = partition_fingerprint();

	zassert_equal(before, after,
		      "attach must not write, or a typo would destroy data");

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a formatted partition.
 * When:  it is attached and detached.
 * Then:  not one byte of the flash has changed, so attaching is safe to do
 *        on a device whose contents matter.
 */
ZTEST(ubi_attach, test_attach_leaves_the_flash_untouched)
{
	uint32_t before = 0;
	uint32_t after = 0;

	zassert_ok(ubi_device_format(&config));

	before = partition_fingerprint();
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
	after = partition_fingerprint();

	zassert_equal(before, after);
}

/*
 * Given: a blank partition.
 * When:  an attach is attempted and fails.
 * Then:  the failure left nothing behind, so a retry starts from the same
 *        place.
 */
ZTEST(ubi_attach, test_failed_attach_on_blank_flash_writes_nothing)
{
	const uint32_t before = partition_fingerprint();

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));

	zassert_equal(before, partition_fingerprint());
}

/*
 * Given: a handle that is already attached.
 * When:  the same handle is attached a second time.
 * Then:  the library refuses rather than leaking the first attachment.
 */
ZTEST(ubi_attach, test_attaching_twice_is_refused)
{
	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EBUSY, ubi_device_init(ubi, &config));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a handle that has been detached.
 * When:  anything is asked of it.
 * Then:  every call refuses, including a second detach.
 */
ZTEST(ubi_attach, test_a_detached_handle_answers_nothing)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(-EINVAL, ubi_device_get_info(ubi, &info));
	zassert_equal(-EINVAL, ubi_device_deinit(ubi));
}

/*
 * Given: the first blank block stamped authentically, but for an image this
 *        device is not, which is what an earlier format leaves behind.
 * When:  the device is attached and one block is reclaimed.
 * Then:  the block is quietly taken back rather than reported as a fault, and
 *        the wear it recorded is carried over rather than forgotten.
 */
ZTEST(ubi_attach, test_a_block_from_an_earlier_image_is_taken_back)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, before.max_erase_count,
		      "a fresh format has stamped every block it touched once");

	stamp_erase_count(config.ikm_key_id, FIRST_BLANK_PNUM,
			  before.image_seq + 1, EARLIER_ERASE_COUNT);

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(0, events_total,
		      "a block left by an earlier image is not a fault");
	zassert_equal(before.healthy_pebs, after.healthy_pebs,
		      "it names another image, so it is not this device's yet");

	/* A reclaim takes the lowest numbered blank block first. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(1, result.performed);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.healthy_pebs + 1, after.healthy_pebs,
		      "once erased and restamped it belongs to this image");
	zassert_equal(EARLIER_ERASE_COUNT + 1, after.max_erase_count,
		      "the erase it recorded has to be carried over, or a "
		      "reformat would hide a worn block");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: nothing but the library.
 * When:  the caller asks how much room a handle needs.
 * Then:  it gets a figure it can allocate, and one larger than the
 *        configuration it will pass in.
 */
ZTEST(ubi_attach, test_the_handle_has_a_size_the_caller_can_allocate)
{
	zassert_not_equal(0, ubi_device_size());
	zassert_true(sizeof(struct ubi_config) < ubi_device_size(),
		     "the handle holds rather more than the configuration");
}
