/**
 * \file    test_trust.c
 * \author  Kamil Kielbasa
 * \brief   Rollback counters, the wear history and the trust verdict.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "partition.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

#if defined(CONFIG_FLASH_SIMULATOR)
/** Rewrites between the copy of the partition and its return. */
#define ROLLBACK_ROUNDS (4)
#endif /* CONFIG_FLASH_SIMULATOR */

/* Enough one-write appends to cross the interval have to fit in one block. */
BUILD_ASSERT(CONFIG_UBI_STATE_CHECK_INTERVAL *UBI_TEST_WRITE_BLOCK <
	     UBI_TEST_PEB_SIZE - UBI_DATA_OFFSET);

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief What an application keeps where the flash cannot reach, to tell
 *        the device it last saw from an older one put in its place.
 */
struct trust_anchor {
	/** Something has been recorded. */
	bool anchored;
	/** The application itself asked for the format the next check sees. */
	bool format_authorised;
	/** Image last seen. */
	uint32_t image_seq;
	/** Highest layout revision seen within it. */
	uint32_t revision;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief The rule docs/security.md recommends: the same image, at a revision
 *        no older than the one last seen.
 */
static enum ubi_state_verdict
trust_the_anchor(const struct ubi_device_info *info, void *user_context);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_trust);

static struct trust_anchor anchor = { 0 };

/* Static function definitions --------------------------------------------- */

static enum ubi_state_verdict
trust_the_anchor(const struct ubi_device_info *info, void *user_context)
{
	ARG_UNUSED(user_context);

	state_check_count += 1;

	if (anchor.anchored && !anchor.format_authorised) {
		if (info->image_seq != anchor.image_seq)
			return UBI_STATE_UNTRUSTED;

		if (info->revision < anchor.revision)
			return UBI_STATE_UNTRUSTED;
	}

	anchor.anchored = true;
	anchor.format_authorised = false;
	anchor.image_seq = info->image_seq;
	anchor.revision = info->revision;

	return UBI_STATE_TRUSTED;
}

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a partition that has just been formatted.
 * When:  it is attached and asked what it counts.
 * Then:  the counters the application anchors rollback detection on describe
 *        exactly what a format writes: each volume table copy stamped once
 *        and sealed once, at the first revision.
 */
ZTEST(ubi_trust, test_the_rollback_counters_start_where_a_format_leaves_them)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.healthy_pebs);
	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.total_erase_count);
	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.max_sqnum);
	zassert_equal(1, info.revision);
}

/*
 * Given: a freshly formatted device.
 * When:  it is attached, which consults the application once.
 * Then:  what the callback was shown is what get_info reports, so a verdict
 *        can be reached on the same figures the application sees.
 */
ZTEST(ubi_trust, test_the_state_check_sees_what_get_info_reports)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(1, state_check_count, "an attach asks exactly once");
	zassert_equal(info.image_seq, last_state.image_seq);
	zassert_equal(info.max_sqnum, last_state.max_sqnum);
	zassert_equal(info.total_erase_count, last_state.total_erase_count);
	zassert_equal(info.healthy_pebs, last_state.healthy_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a formatted device and an application that trusts nothing.
 * When:  an attach is attempted.
 * Then:  it is refused read-only, so refusing to trust the flash is never a
 *        reason to change it.
 */
ZTEST(ubi_trust, test_an_untrusted_state_stops_the_attach)
{
	struct ubi_config guarded = config;

	guarded.state_cb = trust_nothing;

	zassert_ok(ubi_device_format(&config));

	const uint32_t before = partition_fingerprint();

	zassert_equal(-EROFS, ubi_device_init(ubi, &guarded));
	zassert_equal(1, state_check_count);
	zassert_equal(before, partition_fingerprint());

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an application that trusts the attach and nothing after it.
 * When:  appends carry on until the library consults it again.
 * Then:  the refusal stops the writes and latches, so the library refuses
 *        from then on without asking again and without writing.
 */
ZTEST(ubi_trust, test_a_withdrawn_trust_stops_the_writes)
{
	struct ubi_config watched = config;
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t record[UBI_TEST_WRITE_BLOCK] = { 0 };
	uint32_t appended = 0;
	int ret = 0;

	pattern_fill(record, sizeof(record), 0x3C);
	watched.state_cb = trust_once;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &watched));
	zassert_equal(1, state_check_count, "the attach has to ask once");
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	/* Each append is at least one write, so this many cross the
	 * interval. */
	for (; appended < CONFIG_UBI_STATE_CHECK_INTERVAL; ++appended) {
		ret = ubi_leb_write_at(ubi, vol_id, 0,
				       appended * sizeof(record), record,
				       sizeof(record));

		if (0 != ret)
			break;
	}

	zassert_equal(-EROFS, ret, "the interval has to bring the check back");
	zassert_equal(2, state_check_count);

	const uint32_t before = partition_fingerprint();

	zassert_equal(-EROFS, ubi_leb_write_at(ubi, vol_id, 0,
					       appended * sizeof(record),
					       record, sizeof(record)));
	zassert_equal(2, state_check_count, "a latched refusal does not ask");
	zassert_equal(before, partition_fingerprint());

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose trust was withdrawn, with blocks waiting for reclaim.
 * When:  maintenance is asked for.
 * Then:  it is refused, and the result still says that nothing was done and
 *        how much is waiting, as it does on every other return.
 */
ZTEST(ubi_trust, test_a_refused_maintenance_still_reports_the_work)
{
	struct ubi_config watched = config;
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_maintenance_result result = {
		.performed = UINT32_MAX,
		.remaining = UINT32_MAX,
	};
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t record[UBI_TEST_WRITE_BLOCK] = { 0 };

	watched.state_cb = trust_once;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &watched));

	/* Trusted at attach, and refused at the first check after it. */
	int ret = ubi_volume_create(ubi, &wanted, &vol_id);
	uint32_t appended = 0;

	while (0 == ret) {
		ret = ubi_leb_write_at(ubi, vol_id, 0,
				       appended * sizeof(record), record,
				       sizeof(record));
		appended += 1;
	}

	zassert_equal(-EROFS, ret);
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(0 < info.reclaimable_pebs);

	zassert_equal(-EROFS, ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					      &result));
	zassert_equal(0, result.performed);
	zassert_equal(info.reclaimable_pebs, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: the anchors a rollback rule rests on ----------------------------- */

/*
 * Given: a device anchored at attach, then formatted again behind the
 *        application's back.
 * When:  it is attached, and then attached once more after the application
 *        authorised the format itself.
 * Then:  the new image is refused until the application vouches for it.
 */
ZTEST(ubi_trust, test_the_anchor_refuses_an_image_nobody_asked_for)
{
	struct ubi_config anchored = config;

	anchored.state_cb = trust_the_anchor;
	anchor = (struct trust_anchor){ 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_equal(-EROFS, ubi_device_init(ubi, &anchored));

	anchor.format_authorised = true;
	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device anchored after every layout change.
 * When:  a logical block is written and then erased with ubi_leb_erase(),
 *        and the device is attached.
 * Then:  max_sqnum went down, as types.h says it may once the block carrying
 *        the highest number is erased, and the anchor, which does not look
 *        at it, trusts the device all the same.
 */
ZTEST(ubi_trust, test_the_anchor_tolerates_what_normal_use_does)
{
	struct ubi_config anchored = config;
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x4D);
	anchored.state_cb = trust_the_anchor;
	anchor = (struct trust_anchor){ 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_true(after.max_sqnum < before.max_sqnum,
		     "the erase took the highest sequence number with it");
	zassert_equal(before.image_seq, after.image_seq);
	zassert_equal(before.revision, after.revision);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Snapshots bypass the flash driver, which only the simulator allows. */
#if defined(CONFIG_FLASH_SIMULATOR)

/*
 * Given: a device anchored after every layout change, and a copy of the
 *        whole partition taken one change ago.
 * When:  the copy is put back and the device attached.
 * Then:  the older layout is refused, although every byte of it is
 *        authentic.
 */
ZTEST(ubi_trust, test_the_anchor_refuses_a_layout_put_back)
{
	struct ubi_config anchored = config;
	const struct ubi_volume_config first = { .name = "logs",
						 .leb_count = 1 };
	const struct ubi_volume_config second = { .name = "more",
						  .leb_count = 1 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	anchored.state_cb = trust_the_anchor;
	anchor = (struct trust_anchor){ 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_volume_create(ubi, &first, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_volume_create(ubi, &second, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &anchored));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_restore();

	zassert_equal(-EROFS, ubi_device_init(ubi, &anchored));
}

/*
 * Given: a device with data, and a copy of the whole partition taken then.
 * When:  the data is rewritten and reclaimed, the copy is put back and the
 *        device attached.
 * Then:  the image, the revision and the healthy blocks read the same, and
 *        only the erase total and the highest sequence number, both lower,
 *        show that the partition went back in time.
 */
ZTEST(ubi_trust, test_a_data_rollback_shows_only_in_the_wear)
{
	struct ubi_device_info newest = { 0 };
	struct ubi_device_info restored = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x5E);

	zassert_true(0 < pool_ready(&vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	zassert_ok(ubi_device_init(ubi, &config));

	for (uint32_t round = 0; round < ROLLBACK_ROUNDS; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
		zassert_equal(1, result.performed);
	}

	zassert_ok(ubi_device_get_info(ubi, &newest));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_restore();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &restored));

	zassert_equal(newest.image_seq, restored.image_seq);
	zassert_equal(newest.revision, restored.revision);
	zassert_equal(newest.healthy_pebs, restored.healthy_pebs);
	zassert_equal(newest.total_erase_count - ROLLBACK_ROUNDS,
		      restored.total_erase_count,
		      "the erases since the copy are gone with it");
	zassert_equal(newest.max_sqnum - ROLLBACK_ROUNDS, restored.max_sqnum,
		      "and so are the headers written since");

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_FLASH_SIMULATOR */
