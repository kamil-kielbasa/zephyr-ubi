/**
 * \file    partition.c
 * \author  Kamil Kielbasa
 * \brief   The partition under test, read and damaged behind the library's
 *          back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* Test headers: */
#include "flash_faults.h"
#include "partition.h"

/* Module defines ---------------------------------------------------------- */

/** Bytes read at a time when sweeping the partition. */
#define SWEEP_CHUNK (256)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Offset of the next block at or after \p from whose data starts with
 *        \p needle, or -1 when there is none.
 */
static off_t data_find(const struct flash_area *flash_area,
		       const uint8_t *needle, size_t length, off_t from);

/* Module variables and constants ------------------------------------------ */

uint8_t block_scratch[UBI_TEST_PEB_SIZE] = { 0 };

uint8_t saved_blocks[2][UBI_TEST_PEB_SIZE] = { 0 };

/* Static function definitions --------------------------------------------- */

static off_t data_find(const struct flash_area *flash_area,
		       const uint8_t *needle, size_t length, off_t from)
{
	uint8_t chunk[SWEEP_CHUNK] = { 0 };

	/* A shorter comparison would let two payloads look alike. */
	zassert_true(length <= sizeof(chunk),
		     "a needle of %zu bytes is longer than this sweep compares",
		     length);

	for (off_t at = from; at < (off_t)flash_area->fa_size;
	     at += UBI_TEST_PEB_SIZE) {
		zassert_ok(flash_area_read(flash_area, at, chunk, length));

		const bool same = (0 == memcmp(chunk, needle, length));

		if (same)
			return at;
	}

	return -1;
}

/* Module interface function definitions ----------------------------------- */

void partition_geometry_check(void)
{
	const struct flash_area *flash_area = NULL;
	struct flash_pages_info page = { 0 };

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_get_page_info_by_offs(
		flash_area_get_device(flash_area), flash_area->fa_off, &page));

	zassert_equal(UBI_TEST_PEB_SIZE, page.size,
		      "the tests and the driver disagree on the erase block");
	zassert_equal(UBI_TEST_WRITE_BLOCK, flash_area_align(flash_area),
		      "the tests and the driver disagree on the write block");
	zassert_equal(UBI_TEST_PEB_COUNT * UBI_TEST_PEB_SIZE,
		      flash_area->fa_size,
		      "the partition is not a whole number of erase blocks");
	zassert_equal(UBI_TEST_ERASED, flash_area_erased_val(flash_area));

	flash_area_close(flash_area);
}

void partition_fill(uint8_t value)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK] = { 0 };

	memset(chunk, value, sizeof(chunk));

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_erase(flash_area, 0, flash_area->fa_size));

	for (off_t at = 0; at < (off_t)flash_area->fa_size;
	     at += (off_t)sizeof(chunk)) {
		zassert_ok(
			flash_area_write(flash_area, at, chunk, sizeof(chunk)));
	}

	flash_area_close(flash_area);
}

void partition_erase_dirty(void)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK] = { 0 };

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	const uint8_t blank = flash_area_erased_val(flash_area);

	for (off_t block = 0; block < (off_t)flash_area->fa_size;
	     block += UBI_TEST_PEB_SIZE) {
		bool dirty = false;

		for (off_t at = block; at < block + UBI_TEST_PEB_SIZE && !dirty;
		     at += (off_t)sizeof(chunk)) {
			zassert_ok(flash_area_read(flash_area, at, chunk,
						   sizeof(chunk)));

			for (size_t i = 0; i < sizeof(chunk) && !dirty; ++i)
				dirty = (blank != chunk[i]);
		}

		if (dirty) {
			zassert_ok(flash_area_erase(flash_area, block,
						    UBI_TEST_PEB_SIZE));
		}
	}

	flash_area_close(flash_area);
}

uint32_t partition_fingerprint(void)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK] = { 0 };
	uint32_t crc = 0;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	for (off_t at = 0; at < (off_t)flash_area->fa_size;
	     at += (off_t)sizeof(chunk)) {
		zassert_ok(
			flash_area_read(flash_area, at, chunk, sizeof(chunk)));
		crc = crc32_ieee_update(crc, chunk, sizeof(chunk));
	}

	flash_area_close(flash_area);

	return crc;
}

void block_save(uint32_t pnum, uint8_t *buffer)
{
	const struct flash_area *flash_area = NULL;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_read(flash_area, (off_t)pnum * UBI_TEST_PEB_SIZE,
				   buffer, UBI_TEST_PEB_SIZE));
	flash_area_close(flash_area);
}

void block_restore(uint32_t pnum, const uint8_t *buffer)
{
	const struct flash_area *flash_area = NULL;
	const off_t block = (off_t)pnum * UBI_TEST_PEB_SIZE;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_erase(flash_area, block, UBI_TEST_PEB_SIZE));
	zassert_ok(
		flash_area_write(flash_area, block, buffer, UBI_TEST_PEB_SIZE));
	flash_area_close(flash_area);
}

void flash_clear_a_bit(const struct flash_area *flash_area, off_t at)
{
	const off_t block = ROUND_DOWN(at, UBI_TEST_WRITE_BLOCK);
	const size_t index = (size_t)(at - block);
	uint8_t buffer[UBI_TEST_WRITE_BLOCK] = { 0 };

	zassert_ok(flash_area_read(flash_area, block, buffer, sizeof(buffer)));

	/* Otherwise the write changes nothing and the caller counts damage. */
	zassert_not_equal(0x00, buffer[index],
			  "byte at %ld has no bit left to clear", (long)at);

	const uint8_t before = buffer[index];

	buffer[index] &= (uint8_t)(buffer[index] - 1U);

#if defined(CONFIG_FLASH_SIMULATOR)
	flash_faults_damaging(true);
#endif

	zassert_ok(flash_area_write(flash_area, block, buffer, sizeof(buffer)));

#if defined(CONFIG_FLASH_SIMULATOR)
	flash_faults_damaging(false);
#endif

	zassert_ok(flash_area_read(flash_area, block, buffer, sizeof(buffer)));
	zassert_not_equal(before, buffer[index],
			  "the flash did not take the damaged byte");
}

uint32_t corrupt_a_byte_at(const struct flash_area *flash_area, off_t at,
			   size_t length)
{
	uint8_t chunk[SWEEP_CHUNK] = { 0 };
	const size_t reach = MIN(sizeof(chunk), length);

	zassert_ok(flash_area_read(flash_area, at, chunk, reach));

	for (size_t i = 0; i < reach; ++i) {
		if (0x00 == chunk[i])
			continue;

		flash_clear_a_bit(flash_area, at + (off_t)i);

		return 1;
	}

	return 0;
}

uint32_t corrupt_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t damaged = 0;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	off_t at = data_find(flash_area, needle, length, UBI_DATA_OFFSET);

	while (0 <= at) {
		damaged += corrupt_a_byte_at(flash_area, at, length);
		at = data_find(flash_area, needle, length,
			       at + UBI_TEST_PEB_SIZE);
	}

	flash_area_close(flash_area);

	return damaged;
}

uint32_t corrupt_header_of_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t damaged = 0;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	off_t at = data_find(flash_area, needle, length, UBI_DATA_OFFSET);

	while (0 <= at) {
		const off_t vid = at - UBI_DATA_OFFSET + UBI_VID_HEADER_OFFSET;

		/* The checksum covers the whole header, so any byte will do. */
		damaged += corrupt_a_byte_at(flash_area, vid, UBI_HEADER_SIZE);
		at = data_find(flash_area, needle, length,
			       at + UBI_TEST_PEB_SIZE);
	}

	flash_area_close(flash_area);

	return damaged;
}

uint32_t count_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t found = 0;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	off_t at = data_find(flash_area, needle, length, UBI_DATA_OFFSET);

	while (0 <= at) {
		found += 1;
		at = data_find(flash_area, needle, length,
			       at + UBI_TEST_PEB_SIZE);
	}

	flash_area_close(flash_area);

	return found;
}

uint32_t pnum_of_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;

	zassert_equal(1, count_data_matching(needle, length),
		      "exactly one block has to carry those bytes");

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	const off_t at = data_find(flash_area, needle, length, UBI_DATA_OFFSET);

	flash_area_close(flash_area);

	return (uint32_t)((at - UBI_DATA_OFFSET) / UBI_TEST_PEB_SIZE);
}
