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

	event_count = 0;
	memset(event_seen, 0, sizeof(event_seen));

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
