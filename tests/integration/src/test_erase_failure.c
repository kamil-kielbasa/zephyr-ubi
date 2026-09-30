/**
 * \file    test_erase_failure.c
 * \author  Kamil Kielbasa
 * \brief   A block that will not take an erase leaves the device read-only.
 *          Flash simulator only.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "flash_shim.h"
#include "forge.h"
#include "partition.h"
#include "suite.h"
#include "workload.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_erase_failure);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a released block and a flash that has stopped taking erases, which
 *        is how a NOR part usually announces that it is finished.
 * When:  a reclaim step tries to erase it.
 * Then:  the step fails rather than reporting work it did not do, the block
 *        is retired and reported, and the device is read-only until it is
 *        attached again: every write is refused and nothing reaches the
 *        flash, while reads carry on.
 */
ZTEST(ubi_erase_failure,
      test_a_block_that_refuses_an_erase_leaves_the_device_read_only)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const struct ubi_volume_config other = { .name = "other",
						 .leb_count = 1 };
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t other_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x95);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	events_forget();
	flash_fail_erases_after(0);

	zassert_equal(-EIO, ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					    &result));
	zassert_equal(0, result.performed,
		      "a step that failed is not a step performed");

	flash_fail_erases_never();

	const uint32_t fingerprint = partition_fingerprint();

	zassert_equal(-EROFS,
		      ubi_leb_change(ubi, vol_id, 2, written, sizeof(written)));
	zassert_equal(-EROFS, ubi_leb_write_at(ubi, vol_id, 2, 0, written,
					       sizeof(written)));
	zassert_equal(-EROFS, ubi_leb_map(ubi, vol_id, 2));
	zassert_equal(-EROFS, ubi_leb_unmap(ubi, vol_id, 1));
	zassert_equal(-EROFS, ubi_leb_erase(ubi, vol_id, 1));
	zassert_equal(-EROFS, ubi_volume_create(ubi, &other, &other_id));
	zassert_equal(-EROFS, ubi_volume_resize(ubi, vol_id, 1));
	zassert_equal(-EROFS, ubi_volume_remove(ubi, vol_id));
	zassert_equal(-EROFS,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(fingerprint, partition_fingerprint(),
		      "a read-only device wrote to the flash");

	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD],
		      "the application has to hear about a retired block");
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs + 1, after.bad_pebs);
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_change(ubi, vol_id, 2, written, sizeof(written)),
		   "the next attach takes writes again");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block whose earlier copy, replaced by a change or let go of by an
 *        unmap, a reclaim could not erase: the erase failed on a flash that
 *        kept the copy's headers, or the copy could not even be read.
 * When:  the block is erased, and erased again after the next attach.
 * Then:  the first erase is refused rather than reported done over a copy
 *        still on the flash, and the second takes every copy with it.
 */
ZTEST(ubi_erase_failure,
      test_an_erase_is_never_reported_over_a_copy_left_behind)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x71);
	pattern_fill(fresh, sizeof(fresh), 0x72);

	for (uint32_t fault = 0; fault < 3; ++fault) {
		const bool unreadable = (0 != fault);
		const bool unmapped = (2 == fault);
		const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

		zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
					   info.reclaimable_pebs, &result));

		if (unmapped) {
			zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
		} else {
			zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh,
						  sizeof(fresh)));
		}

		/* The old copy is the one block left waiting. */
		if (unreadable) {
			flash_fail_reads_of(
				pnum_of_data_matching(old, sizeof(old)));
		} else {
			flash_refuse_overwrites();
			flash_fail_erases_after(0);
		}

		zassert_equal(-EIO,
			      ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					      &result));

		flash_faults_clear();

		zassert_equal(1, count_data_matching(old, sizeof(old)));
		zassert_equal(-EROFS, ubi_leb_erase(ubi, vol_id, 0),
			      "an erase was reported over a copy still on "
			      "the flash");

		zassert_ok(ubi_device_deinit(ubi));
		zassert_ok(ubi_device_init(ubi, &config));

		zassert_ok(ubi_leb_erase(ubi, vol_id, 0));
		zassert_equal(0, count_data_matching(old, sizeof(old)));
		zassert_equal(0, count_data_matching(fresh, sizeof(fresh)));

		zassert_ok(ubi_device_deinit(ubi));
		zassert_ok(ubi_device_init(ubi, &config));

		zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &leb));
		zassert_false(leb.mapped,
			      "no copy may come back after a reboot");

		zassert_ok(ubi_device_deinit(ubi));
	}
}

/*
 * Given: a block whose erase counter holds as much as it can.
 * When:  its logical block is erased.
 * Then:  the erase fails rather than let the counter wrap, and the device is
 *        read-only.
 */
ZTEST(ubi_erase_failure,
      test_an_erase_counter_at_its_limit_leaves_the_device_read_only)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x73);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	erase_count_rewrite(config.ikm_key_id,
			    pnum_of_data_matching(written, sizeof(written)),
			    info.image_seq, UBI_MAX_ERASE_COUNT);

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EINVAL, ubi_leb_erase(ubi, vol_id, 0));
	zassert_equal(-EROFS,
		      ubi_leb_change(ubi, vol_id, 1, written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device with no block free, and a flash that stops taking erases.
 * When:  a volume is created, which needs a block erased first.
 * Then:  the create fails with the table as it was, having tried one block:
 *        the device is read-only from the first failed erase on.
 */
ZTEST(ubi_erase_failure,
      test_a_table_update_that_cannot_get_a_block_stops_there)
{
	const struct ubi_volume_config second = { .name = "second",
						  .leb_count = 1 };
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t created = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;

	zassert_true(0 < volume_under_load(&vol_id));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_equal(0, before.free_pebs, "the table has to erase a block");

	events_forget();
	flash_fail_erases_after(0);

	zassert_equal(-EIO, ubi_volume_create(ubi, &second, &created));

	flash_fail_erases_never();

	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD],
		      "another block was tried on a read-only device");
	zassert_equal(-ENOENT, ubi_volume_find(ubi, second.name, &found));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.revision, after.revision);
	zassert_equal(-EROFS, ubi_volume_create(ubi, &second, &created));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose flash stops taking erases.
 * When:  a volume is created, and the table copy the first new one replaces
 *        cannot be erased.
 * Then:  the new table stands in the copy already written and the device is
 *        read-only, with the other copy left for the next attach; after it,
 *        the volume is there and a repair brings back the second copy.
 */
ZTEST(ubi_erase_failure, test_a_table_update_that_cannot_erase_keeps_one_copy)
{
	const struct ubi_volume_config second = { .name = "second",
						  .leb_count = 1 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t created = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;

	zassert_not_equal(UBI_VOL_ID_INVALID,
			  volume_ready(UBI_TEST_VOLUME_LEBS));

	events_forget();
	flash_fail_erases_after(0);

	zassert_ok(ubi_volume_create(ubi, &second, &created));

	flash_fail_erases_never();

	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "the second copy was never written");
	zassert_equal(-EROFS, ubi_volume_remove(ubi, created));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_find(ubi, second.name, &found));
	zassert_equal(created, found);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 0, &result));
	zassert_equal(1, result.remaining, "the table is down to one copy");
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(0, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "the repair left the table in two copies");
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block retired by a write fault, and a flash that has since
 *        stopped taking erases.
 * When:  a repair tries to bring the block back.
 * Then:  the repair fails rather than write the block off as an answer, and
 *        the device is read-only, with the block still waiting.
 */
ZTEST(ubi_erase_failure,
      test_a_repair_that_cannot_erase_leaves_the_device_read_only)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_maintenance_result result = { 0 };

	retire_one_block(vol_id, 0);

	flash_fail_erases_after(0);

	zassert_equal(-EIO,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));

	flash_fail_erases_never();

	zassert_equal(0, result.performed);
	zassert_equal(1, result.remaining);
	zassert_equal(-EROFS,
		      ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: cold blocks worth relocating, and a flash that stops taking erases.
 * When:  relocation runs with a budget of two, and the block the first move
 *        leaves cannot be erased.
 * Then:  that move stands and nothing more is moved: the device is read-only
 *        from then on, and every block still reads what it held, before the
 *        next attach and after it.
 */
ZTEST(ubi_erase_failure, test_a_relocation_that_cannot_erase_its_source_stops)
{
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0xE5);

	const uint32_t vol_id =
		unevenly_worn_device(cold, sizeof(cold), &leb_count);

	/* The worn block goes back to the pool as well, so that a second move
	 * has somewhere to go. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 2, &result));
	zassert_equal(2, result.performed, "two moves are due");
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_restore();

	zassert_ok(ubi_device_init(ubi, &config));

	events_forget();
	flash_fail_erases_after(0);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 2, &result));

	flash_fail_erases_never();

	zassert_equal(1, result.performed,
		      "a block was moved after the device turned read-only");
	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);
	zassert_equal(-EROFS, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1,
					      &result));
	cold_blocks_check(vol_id, leb_count, cold, sizeof(cold));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	cold_blocks_check(vol_id, leb_count, cold, sizeof(cold));

	zassert_ok(ubi_device_deinit(ubi));
}
