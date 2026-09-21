/**
 * \file    test_integrity.c
 * \author  Kamil Kielbasa
 * \brief   What a damaged or tampered partition does to an attach.
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

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_integration, test_one_damaged_volume_table_copy_is_survived)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_volume_tables(ubi, &config, 1));

	/* This is what the second copy is for, but the device is now one
	 * erase away from losing a revision and has to say so. */
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(before.image_seq, after.image_seq);
	zassert_equal(before.revision, after.revision);
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	/* The record carries a CRC in its sealed header, so a changed byte is
	 * caught before the MAC is ever checked. Which of the two it was
	 * cannot be told from out here, and the report does not pretend. */
	zassert_equal(1, event_seen[UBI_EVENT_VOLUME_TABLE_CORRUPT]);
	zassert_true(event_last[UBI_EVENT_VOLUME_TABLE_CORRUPT].pnum <
			     before.peb_count,
		     "the report has to name the block it came from");
	zassert_false(event_seen[UBI_EVENT_HDR_TAMPERED],
		      "the headers were not touched");
}

ZTEST(ubi_integration, test_both_damaged_volume_table_copies_lose_the_device)
{
	zassert_ok(ubi_device_format(&config));
	zassert_equal(2, corrupt_volume_tables(ubi, &config, 2));

	/*
	 * The record carries a checksum written before it, so a changed byte
	 * is indistinguishable from a write that never finished. Either way
	 * there is no usable volume table left, which is what attach reports.
	 */
	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

ZTEST(ubi_integration, test_erasing_every_stamped_block_loses_the_device)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	for (uint32_t pnum = 0; pnum < info.peb_count; ++pnum) {
		uint8_t magic[4] = { 0 };

		zassert_ok(flash_area_read(flash_area,
					   (off_t)pnum * info.peb_size, magic,
					   sizeof(magic)));

		if (0xFF == magic[0])
			continue;

		zassert_ok(flash_area_erase(flash_area,
					    (off_t)pnum * info.peb_size,
					    info.peb_size));
	}

	flash_area_close(flash_area);

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

ZTEST(ubi_integration, test_a_damaged_erase_counter_header_is_reported)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };
	uint8_t byte = 0;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	/* A freshly stamped block has an erase count of one, so the last byte
	 * of that field has a bit NOR still allows clearing. */
	for (uint32_t pnum = 0; pnum < info.peb_count; ++pnum) {
		const off_t at = (off_t)pnum * info.peb_size + 0x0F;

		zassert_ok(flash_area_read(flash_area, at, &byte, 1));

		if (0x01 == byte) {
			flash_clear_a_bit(flash_area, at);
			break;
		}
	}

	flash_area_close(flash_area);

	events_forget();

	/* The other copy carries the device through, but the damage is still
	 * reported. Nobody repaired the checksum, so it is damage rather than
	 * tampering. */
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_true(event_seen[UBI_EVENT_HDR_CORRUPT]);
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		     "one copy left has to be reported as such");
	zassert_false(event_seen[UBI_EVENT_HDR_TAMPERED]);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.healthy_pebs, "the damaged block is not counted");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_damaged_block_that_still_holds_data_is_kept)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = 4 };
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[64];

	memset(written, 0xD7, sizeof(written));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_header_of_data_matching(written,
							 sizeof(written)));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(1, info.corrupt_pebs,
		      "a block whose data survived its header is preserved");

	/* Neither pass may take it away: erasing it would destroy the only
	 * copy of data the application may still want to salvage. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 16, &result));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 16, &result));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(1, info.corrupt_pebs,
		      "maintenance must not erase what it cannot read");

	zassert_ok(ubi_device_deinit(ubi));
}
