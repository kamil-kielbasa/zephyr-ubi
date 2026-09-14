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
#include <zephyr/sys/util.h>
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

void flash_clear_a_bit(const struct flash_area *flash_area, off_t at)
{
	const off_t block = ROUND_DOWN(at, UBI_TEST_WRITE_BLOCK);
	const size_t index = (size_t)(at - block);
	uint8_t buffer[UBI_TEST_WRITE_BLOCK];

	zassert_equal(UBI_TEST_WRITE_BLOCK, flash_area_align(flash_area),
		      "the overlay and the driver have to agree");
	zassert_ok(flash_area_read(flash_area, block, buffer, sizeof(buffer)));

	buffer[index] &= (uint8_t)(buffer[index] - 1U);

	/* The bytes around it go back bit for bit, which NOR always allows. */
	zassert_ok(flash_area_write(flash_area, block, buffer, sizeof(buffer)));
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

	/* The record sits behind the two headers of whichever block holds it. */
	for (uint32_t pnum = 0; pnum < info.peb_count && damaged < copies;
	     ++pnum) {
		const off_t at = (off_t)pnum * info.peb_size + 128;

		zassert_ok(flash_area_read(flash_area, at, &byte, 1));

		if (0xFF != byte && 0x00 != byte) {
			flash_clear_a_bit(flash_area, at);
			damaged += 1;
		}
	}

	flash_area_close(flash_area);

	return damaged;
}

static off_t data_find(const struct flash_area *flash_area,
		       const uint8_t *needle, size_t length, off_t from)
{
	uint8_t chunk[SWEEP_CHUNK];
	const size_t compared = MIN(sizeof(chunk), length);

	for (off_t at = from; at < (off_t)flash_area->fa_size;
	     at += UBI_TEST_PEB_SIZE) {
		zassert_ok(flash_area_read(flash_area, at, chunk, compared));

		if (0 == memcmp(chunk, needle, compared))
			return at;
	}

	return -1;
}

uint32_t count_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t found = 0;
	off_t at = UBI_TEST_DATA_OFFSET;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	while (0 <= (at = data_find(flash_area, needle, length, at))) {
		found += 1;
		at += UBI_TEST_PEB_SIZE;
	}

	flash_area_close(flash_area);

	return found;
}

uint32_t corrupt_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t damaged = 0;
	off_t at = UBI_TEST_DATA_OFFSET;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	while (0 <= (at = data_find(flash_area, needle, length, at))) {
		flash_clear_a_bit(flash_area, at);
		damaged += 1;
		at += UBI_TEST_PEB_SIZE;
	}

	flash_area_close(flash_area);

	return damaged;
}
