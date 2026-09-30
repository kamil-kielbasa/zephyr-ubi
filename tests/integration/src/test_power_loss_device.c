/**
 * \file    test_power_loss_device.c
 * \author  Kamil Kielbasa
 * \brief   Every point at which a volume table update, a move of a table
 *          copy or a format can fail or lose the power. Flash simulator
 *          only.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "forge.h"
#include "partition.h"
#include "suite.h"
#include "sweep.h"
#include "table_copies.h"

/* Module defines ---------------------------------------------------------- */

/** Erase count that leaves the volume table copies the least worn blocks by
 *  far. */
#define TABLE_WEAR_GAP (4 * CONFIG_UBI_WEAR_LEVELING_THRESHOLD)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Stamp every block but the volume table copies an attach could adopt
 *        with an erase count far past theirs, so that levelling moves a
 *        copy before anything else.
 */
static void table_made_coldest(uint32_t image_seq);

/**
 * \brief Report whether the flash holds two volume table copies an attach
 *        could adopt, of different revisions.
 */
static bool table_copies_disagree(void);

/**
 * \brief Report whether the flash holds two volume table copies an attach
 *        could adopt, both at \p revision.
 */
static bool table_copies_at(uint32_t revision);

/**
 * \brief Cut short the move of the volume table copy levelling reaches
 *        first, at every point along the way, and check every reboot.
 *
 * \param one_copy                      Whether the device is down to one
 *                                      copy before the move.
 */
static void table_move_cut_short(bool one_copy);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_power_loss_device);

/* Static function definitions --------------------------------------------- */

static void table_made_coldest(uint32_t image_seq)
{
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	const uint32_t copies = volume_table_adoptable(config.ikm_key_id, pnums,
						       NULL, ARRAY_SIZE(pnums));

	zassert_true(copies <= ARRAY_SIZE(pnums));

	for (uint32_t pnum = 0; pnum < UBI_TEST_PEB_COUNT; ++pnum) {
		bool copy = false;

		for (uint32_t i = 0; i < copies; ++i)
			copy = copy || (pnums[i] == pnum);

		if (!copy)
			stamp_erase_count(config.ikm_key_id, pnum, image_seq,
					  TABLE_WEAR_GAP);
	}
}

static bool table_copies_disagree(void)
{
	uint32_t revisions[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	const uint32_t copies = volume_table_adoptable(
		config.ikm_key_id, NULL, revisions, ARRAY_SIZE(revisions));

	return UBI_VOLUME_TABLE_LEB_COUNT == copies &&
	       revisions[0] != revisions[1];
}

static bool table_copies_at(uint32_t revision)
{
	uint32_t revisions[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	const uint32_t copies = volume_table_adoptable(
		config.ikm_key_id, NULL, revisions, ARRAY_SIZE(revisions));

	return UBI_VOLUME_TABLE_LEB_COUNT == copies &&
	       revision == revisions[0] && revision == revisions[1];
}

static void table_move_cut_short(bool one_copy)
{
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };

	zassert_not_equal(UBI_VOL_ID_INVALID,
			  volume_ready(UBI_TEST_VOLUME_LEBS));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	/* The first copy goes, so that the move lands below the one left and
	 * an attach meets the moved copy first. */
	if (one_copy) {
		const struct flash_area *flash_area = NULL;

		zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT,
			      volume_table_blocks(config.ikm_key_id, pnums,
						  ARRAY_SIZE(pnums)));
		zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
		zassert_ok(flash_area_erase(flash_area,
					    (off_t)pnums[0] * UBI_TEST_PEB_SIZE,
					    UBI_TEST_PEB_SIZE));
		flash_area_close(flash_area);
	}

	table_made_coldest(before.image_seq);
	flash_snapshot_take();

	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			flash_snapshot_restore();
			sweep_attach();

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret = ubi_maintenance(
				ubi, UBI_MAINTENANCE_RELOCATE, 1, &result);
			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the move failed with %d", ret);

			/* Nothing but the table is mapped. */
			if (!cut_short)
				zassert_equal(1, result.performed);

			sweep_reboot();
			events_forget();

			zassert_ok(ubi_device_init(ubi, &config),
				   "a cut %s %u lost the device", where, cut);
			zassert_ok(ubi_device_get_info(ubi, &after));
			zassert_equal(before.revision, after.revision);
			zassert_true(volume_present("logs"));

			/* Down to one copy, the attach says so either way. */
			const uint32_t degradations =
				event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED];

			if (!one_copy)
				zassert_equal(0, degradations,
					      "a cut %s %u cost a table copy",
					      where, cut);

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;
		}
	}
}

/* Module interface function definitions ----------------------------------- */

/* Tests: a volume table update -------------------------------------------- */

/*
 * Given: a device holding one volume with data in it.
 * When:  creating a second one is cut short by a power loss, at every point
 *        along the way, in a write and in an erase.
 * Then:  the device always attaches again with the first volume and its
 *        data intact, and the second one exists once the update has gone
 *        through.
 */
ZTEST(ubi_power_loss_device, test_a_volume_update_cut_short_keeps_the_device)
{
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint32_t created = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x57);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			flash_snapshot_restore();
			sweep_attach();

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret =
				ubi_volume_create(ubi, &second, &created);
			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the update failed with %d", ret);

			sweep_reboot();

			zassert_ok(ubi_device_init(ubi, &config),
				   "a cut %s %u lost the device", where, cut);
			zassert_true(volume_present("logs"));
			leb_check(vol_id, 0, written, sizeof(written));

			if (!cut_short)
				zassert_true(volume_present(second.name));

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;
		}
	}
}

/*
 * Given: a device holding one volume, with both volume table copies.
 * When:  creating a second one is cut short by a power loss, at every point
 *        along the way.
 * Then:  some cuts leave the update out and some let it in. Whenever it did
 *        not take, the table it had is still in both copies, as no copy is
 *        touched before the one replacing it is down; whenever it took, the
 *        second volume is there.
 */
ZTEST(ubi_power_loss_device,
      test_an_update_that_did_not_take_leaves_both_copies)
{
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t created = UBI_VOL_ID_INVALID;
	uint32_t not_taken = 0;
	uint32_t taken = 0;

	zassert_not_equal(UBI_VOL_ID_INVALID,
			  volume_ready(UBI_TEST_VOLUME_LEBS));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			flash_snapshot_restore();
			sweep_attach();

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret =
				ubi_volume_create(ubi, &second, &created);
			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the update failed with %d", ret);

			sweep_reboot();
			sweep_attach();
			zassert_ok(ubi_device_get_info(ubi, &after));

			const bool present = volume_present(second.name);

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;

			if (before.revision != after.revision) {
				taken += 1;
				zassert_true(present,
					     "a cut %s %u took the update "
					     "without the volume",
					     where, cut);
				continue;
			}

			not_taken += 1;
			zassert_false(present);
			zassert_true(table_copies_at(before.revision),
				     "a cut %s %u cost a table copy", where,
				     cut);
		}
	}

	zassert_true(0 < not_taken, "every cut let the update through");
	zassert_true(0 < taken, "no cut let the update through");
}

/*
 * Given: a device holding one volume, with both volume table copies in place
 *        or with one of them already lost.
 * When:  creating a second volume fails on one write, at every point along
 *        the way, and the device carries on.
 * Then:  the volumes the device reports right after the failure are the
 *        ones the next attach finds, the device goes on writing without
 *        writing twice over the same bytes, and a table left with one copy
 *        still asks to be repaired.
 */
ZTEST(ubi_power_loss_device,
      test_a_failed_volume_update_leaves_ram_and_flash_agreeing)
{
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t created = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x68);

	for (uint32_t degraded = 0; degraded < 2; ++degraded) {
		partition_erase_dirty();

		const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

		zassert_ok(ubi_device_deinit(ubi));

		if (1 == degraded) {
			const struct flash_area *flash_area = NULL;

			zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT,
				      volume_table_blocks(config.ikm_key_id,
							  pnums,
							  ARRAY_SIZE(pnums)));
			zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID,
						   &flash_area));
			zassert_ok(flash_area_erase(
				flash_area, (off_t)pnums[1] * UBI_TEST_PEB_SIZE,
				UBI_TEST_PEB_SIZE));
			flash_area_close(flash_area);
		}

		flash_snapshot_take();

		for (uint32_t budget = 0;; budget += SWEEP_STRIDE) {
			struct ubi_maintenance_result result = { 0 };

			flash_snapshot_restore();
			sweep_attach();
			events_forget();

			flash_fail_one_write_after(budget);
			const int ret =
				ubi_volume_create(ubi, &second, &created);
			const bool failed = flash_fault_fired();

			flash_faults_clear();

			const bool in_ram = volume_present(second.name);

			zassert_equal(0 == ret, in_ram,
				      "a failure after %u bytes returned %d",
				      budget, ret);

			/* A failed write clearing a header before an erase
			 * costs nothing, so only these leave work behind. */
			const bool work_left =
				0 < event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED] ||
				0 < event_count[UBI_EVENT_PEB_BAD];

			if (work_left) {
				zassert_ok(ubi_maintenance(
					ubi, UBI_MAINTENANCE_REPAIR, 0,
					&result));
				zassert_true(0 < result.remaining,
					     "a failure after %u bytes left "
					     "nothing to repair",
					     budget);
			}

			zassert_ok(ubi_leb_change(ubi, vol_id, 1, written,
						  sizeof(written)));

			sweep_reboot();
			sweep_attach();

			zassert_equal(in_ram, volume_present(second.name),
				      "a failure after %u bytes split RAM from "
				      "flash",
				      budget);
			leb_check(vol_id, 1, written, sizeof(written));

			zassert_ok(ubi_device_deinit(ubi));

			if (!failed)
				break;
		}
	}
}

/* Tests: a volume table copy on the move ---------------------------------- */

/*
 * Given: a volume update cut short after its first volume table copy went
 *        down and before the second was touched, so the flash holds the new
 *        table in one copy and the old one in the other, on a device where
 *        levelling reaches the table copies first.
 * When:  levelling moves a block, and the device is attached again.
 * Then:  the update is still there: the old copy never comes back.
 */
ZTEST(ubi_power_loss_device, test_a_table_copy_left_behind_never_comes_back)
{
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t created = UBI_VOL_ID_INVALID;
	uint32_t left_behind = 0;

	zassert_not_equal(UBI_VOL_ID_INVALID,
			  volume_ready(UBI_TEST_VOLUME_LEBS));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	/* The old copy is left whole by a cut between two writes when its
	 * headers are cleared before the erase, and by a cut inside the erase
	 * when they are not. */
	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			flash_snapshot_restore();
			sweep_attach();

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret =
				ubi_volume_create(ubi, &second, &created);
			const bool cut_short = flash_power_is_cut();

			sweep_reboot();

			if (!cut_short) {
				zassert_ok(ret);
				break;
			}

			const bool disagree = table_copies_disagree();

			if (!disagree)
				continue;

			left_behind += 1;
			table_made_coldest(info.image_seq);

			sweep_attach();
			zassert_true(volume_present(second.name));
			zassert_ok(ubi_maintenance(
				ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
			zassert_equal(1, result.performed);

			sweep_reboot();
			sweep_attach();

			zassert_true(volume_present(second.name),
				     "a cut %s %u and a relocation undid the "
				     "update",
				     where, cut);

			zassert_ok(ubi_device_deinit(ubi));
		}
	}

	zassert_true(0 < left_behind, "no cut left the old copy whole");
}

/*
 * Given: a device with both volume table copies, on which levelling reaches
 *        a table copy first.
 * When:  moving that copy is cut short by a power loss, at every point along
 *        the way.
 * Then:  the device attaches again with the table it had, in both copies.
 */
ZTEST(ubi_power_loss_device, test_moving_a_table_copy_cut_short_costs_no_copy)
{
	table_move_cut_short(false);
}

/*
 * Given: a device down to one volume table copy, on which levelling reaches
 *        that copy first.
 * When:  moving it is cut short by a power loss, at every point along the
 *        way.
 * Then:  the device attaches again with the table it had.
 */
ZTEST(ubi_power_loss_device, test_moving_the_last_table_copy_cut_short_keeps_it)
{
	table_move_cut_short(true);
}

/* Tests: a format --------------------------------------------------------- */

/*
 * Given: a partition carrying a device with a volume and data on it.
 * When:  it is formatted again, and the format is cut short by a power loss
 *        at every point along the way.
 * Then:  the partition always attaches, as the old device or as the new
 *        one, and never as something in between.
 */
ZTEST(ubi_power_loss_device, test_a_format_cut_short_leaves_a_whole_device)
{
	struct ubi_device_info info = { 0 };
	struct ubi_device_info old = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	pattern_fill(written, sizeof(written), 0x9B);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &old));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t cut = 0;; cut += SWEEP_STRIDE) {
		flash_snapshot_restore();

		flash_power_cut_after(cut);
		const int ret = ubi_device_format(&config);
		const bool cut_short = flash_power_is_cut();

		flash_faults_clear();

		zassert_ok(ubi_device_init(ubi, &config),
			   "a cut after %u bytes left no device", cut);
		zassert_ok(ubi_device_get_info(ubi, &info));

		if (info.image_seq == old.image_seq) {
			zassert_true(cut_short);
			leb_check(vol_id, 0, written, sizeof(written));
		} else {
			zassert_equal(0, info.volume_count);
		}

		zassert_ok(ubi_device_deinit(ubi));

		if (!cut_short) {
			zassert_ok(ret);
			zassert_not_equal(old.image_seq, info.image_seq);
			break;
		}
	}
}
