/**
 * \file    common.c
 * \author  Kamil Kielbasa
 * \brief   Reaching the flash behind the library's back.
 *
 *          The library carries no test hooks; where a test needs to see or
 *          damage the flash it opens the partition itself, exactly as an
 *          attacker or a stray writer would.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"

/* Module defines ---------------------------------------------------------- */

/** Chunk used when sweeping the whole partition. */
#define SWEEP_CHUNK (256)

/* Module interface function definitions ----------------------------------- */

void partition_fill(uint8_t value)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK];

	memset(chunk, value, sizeof(chunk));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));
	zassert_ok(flash_area_erase(flash_area, 0, flash_area->fa_size));

	if (0xFF != value) {
		for (off_t at = 0; at < (off_t)flash_area->fa_size;
		     at += sizeof(chunk)) {
			zassert_ok(flash_area_write(flash_area, at, chunk,
						    sizeof(chunk)));
		}
	}

	flash_area_close(flash_area);
}

uint32_t partition_fingerprint(void)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK];
	uint32_t crc = 0;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	for (off_t at = 0; at < (off_t)flash_area->fa_size;
	     at += sizeof(chunk)) {
		zassert_ok(
			flash_area_read(flash_area, at, chunk, sizeof(chunk)));
		crc = crc32_ieee_update(crc, chunk, sizeof(chunk));
	}

	flash_area_close(flash_area);

	return crc;
}

uint32_t corrupt_volume_tables(struct ubi_device *ubi,
			       const struct ubi_config *config, uint32_t copies)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };
	uint8_t byte = 0;
	uint32_t damaged = 0;

	zassert_ok(ubi_device_init(ubi, config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	/* The record sits behind the two headers of whichever block holds it;
	 * clearing the lowest set bit is a write NOR always allows. */
	for (uint32_t pnum = 0; pnum < info.peb_count && damaged < copies;
	     ++pnum) {
		const off_t at = (off_t)pnum * info.peb_size + 128;

		zassert_ok(flash_area_read(flash_area, at, &byte, 1));

		if (0xFF != byte && 0x00 != byte) {
			byte &= (uint8_t)(byte - 1U);
			zassert_ok(flash_area_write(flash_area, at, &byte, 1));
			damaged += 1;
		}
	}

	flash_area_close(flash_area);

	return damaged;
}
