/**
 * \file    common.c
 * \author  Kamil Kielbasa
 * \brief   Reaching the flash behind the library's back.
 *
 *          The library carries no test hooks; a test that needs to see or
 *          damage the flash opens the partition itself.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#if defined(CONFIG_FLASH_SIMULATOR)
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/stats/stats.h>
#endif

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_header.h"
#include "ubi_key.h"
#include "ubi_volume_table.h"

/* Test headers: */
#include "common.h"

/* Module defines ---------------------------------------------------------- */

/** Bytes read at a time when sweeping the partition. */
#define SWEEP_CHUNK (256)

/** The checksum is the last word of either header. */
#define HEADER_CRC_OFFSET (UBI_HEADER_SIZE - sizeof(uint32_t))

#if defined(CONFIG_FLASH_SIMULATOR)

/** Room for the longest name the simulator registers, "erase_cycles_unit255". */
#define COUNTER_NAME_SIZE (32)

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module type definitions ------------------------------------------------- */

#if defined(CONFIG_FLASH_SIMULATOR)

/** One counter being looked for while walking the statistics group. */
struct counter_wanted {
	/** Name to match. */
	const char *name;
	/** What it held. */
	uint32_t value;
	/** Whether the name was found at all. */
	bool found;
};

#endif /* CONFIG_FLASH_SIMULATOR */

/* Static function declarations -------------------------------------------- */

/**
 * \brief Offset of the next block at or after \p from whose data starts with
 *        \p needle, or -1 when there is none.
 */
static off_t data_find(const struct flash_area *flash_area,
		       const uint8_t *needle, size_t length, off_t from);

/**
 * \brief Clear a bit in the first byte of a range that still has one.
 *
 * \return 1 when a byte was damaged, 0 when every byte was already zero.
 */
static uint32_t corrupt_a_byte_at(const struct flash_area *flash_area, off_t at,
				  size_t length);

#if defined(CONFIG_FLASH_SIMULATOR)

/**
 * \brief Refuse a write once the allowance of flash_fail_writes_after() is
 *        spent.
 */
static int write_byte_fails(const struct device *dev, off_t offset,
			    uint8_t data);

/**
 * \brief Refuse an erase once the allowance of flash_fail_erases_after() is
 *        spent, and carry it out by hand otherwise.
 */
static int erase_unit_fails(const struct device *dev, off_t unit_offset);

/**
 * \brief Install the injected faults while any is armed, remove them after.
 */
static void faults_apply(void);

/**
 * \brief Pick the wanted counter out of the statistics group being walked.
 */
static int counter_match(struct stats_hdr *group, void *arg, const char *name,
			 uint16_t offset);

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module variables and constants ------------------------------------------ */

/** Room to hold one block while its header is resealed. */
static uint8_t block_scratch[UBI_TEST_PEB_SIZE] = { 0 };

#if defined(CONFIG_FLASH_SIMULATOR)

/** Bytes the injected write fault still lets through. */
static uint32_t writes_left = 0;

/** Whether the write fault is armed. */
static bool writes_fail = false;

/** Erases the injected erase fault still lets through. */
static uint32_t erases_left = 0;

/** Whether the erase fault is armed. */
static bool erases_fail = false;

/** Callbacks the simulator runs while a fault is armed. */
static const struct flash_simulator_cb injected_faults = {
	.write_byte = write_byte_fails,
	.erase_unit = erase_unit_fails,
};

#endif /* CONFIG_FLASH_SIMULATOR */

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

		if (0 == memcmp(chunk, needle, length))
			return at;
	}

	return -1;
}

static uint32_t corrupt_a_byte_at(const struct flash_area *flash_area, off_t at,
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

#if defined(CONFIG_FLASH_SIMULATOR)

static int write_byte_fails(const struct device *dev, off_t offset,
			    uint8_t data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(offset);

	if (writes_fail) {
		if (0 == writes_left)
			return -EIO;

		writes_left -= 1;
	}

	return data;
}

static int erase_unit_fails(const struct device *dev, off_t unit_offset)
{
	size_t size = 0;
	uint8_t *memory = NULL;

	if (erases_fail) {
		if (0 == erases_left)
			return -EIO;

		erases_left -= 1;
	}

	memory = flash_simulator_get_memory(dev, &size);

	zassert_not_null(memory, "the simulator has no memory to erase");
	zassert_true((size_t)unit_offset + UBI_TEST_PEB_SIZE <= size);

	/* The simulator stops erasing on its own once this callback exists. */
	memset(&memory[unit_offset], flash_get_parameters(dev)->erase_value,
	       UBI_TEST_PEB_SIZE);

	return 0;
}

static void faults_apply(void)
{
	const struct flash_area *flash_area = NULL;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	flash_simulator_set_callbacks(
		flash_area_get_device(flash_area),
		(writes_fail || erases_fail) ? &injected_faults : NULL);

	flash_area_close(flash_area);
}

static int counter_match(struct stats_hdr *group, void *arg, const char *name,
			 uint16_t offset)
{
	struct counter_wanted *wanted = arg;
	const uint8_t *entries = (const uint8_t *)group;

	if (0 != strcmp(wanted->name, name))
		return 0;

	zassert_equal(sizeof(wanted->value), group->s_size,
		      "counter \"%s\" is not 32 bits wide", name);

	memcpy(&wanted->value, &entries[offset], sizeof(wanted->value));
	wanted->found = true;

	return 0;
}

#endif /* CONFIG_FLASH_SIMULATOR */

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

void stamp_erase_count(psa_key_id_t ikm_key_id, uint32_t pnum,
		       uint32_t image_seq, uint64_t erase_count)
{
	const struct flash_area *flash_area = NULL;
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };
	const off_t block = (off_t)pnum * UBI_TEST_PEB_SIZE;

	const struct ubi_ec_header header = {
		.erase_count = erase_count,
		.image_seq = image_seq,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};

	zassert_ok(ubi_impl_key_derive(ikm_key_id, &key_header,
				       &key_volume_table));
	zassert_ok(ubi_impl_header_ec_serialize(&header, key_header, pnum,
						buffer, sizeof(buffer)));

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_erase(flash_area, block, UBI_TEST_PEB_SIZE));
	zassert_ok(flash_area_write(flash_area, block, buffer, sizeof(buffer)));

	flash_area_close(flash_area);
}

void erase_counts_on_flash(psa_key_id_t ikm_key_id, uint64_t *counts,
			   uint32_t peb_count)
{
	const struct flash_area *flash_area = NULL;
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_ec_header header = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_key_derive(ikm_key_id, &key_header,
				       &key_volume_table));
	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	const uint8_t blank = flash_area_erased_val(flash_area);

	for (uint32_t pnum = 0; pnum < peb_count; ++pnum) {
		zassert_ok(flash_area_read(flash_area,
					   (off_t)pnum * UBI_TEST_PEB_SIZE,
					   buffer, sizeof(buffer)));
		zassert_equal(UBI_HEADER_OK,
			      ubi_impl_header_ec_parse(buffer, sizeof(buffer),
						       key_header, pnum, blank,
						       &header),
			      "block %u carries no erase counter header", pnum);

		counts[pnum] = header.erase_count;
	}

	flash_area_close(flash_area);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);
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

	zassert_ok(flash_area_write(flash_area, block, buffer, sizeof(buffer)));
	zassert_ok(flash_area_read(flash_area, block, buffer, sizeof(buffer)));
	zassert_not_equal(before, buffer[index],
			  "the flash did not take the damaged byte");
}

void forge_header_byte(uint32_t pnum, off_t at)
{
	const struct flash_area *flash_area = NULL;
	const off_t block = (off_t)pnum * UBI_TEST_PEB_SIZE;

	zassert_true(at < UBI_DATA_OFFSET,
		     "only a header carries a checksum to repair");

	const off_t header =
		(at < UBI_VID_HEADER_OFFSET) ? 0 : UBI_VID_HEADER_OFFSET;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_read(flash_area, block, block_scratch,
				   sizeof(block_scratch)));

	block_scratch[at] ^= 0x01;
	sys_put_be32(crc32_ieee(&block_scratch[header], HEADER_CRC_OFFSET),
		     &block_scratch[header + HEADER_CRC_OFFSET]);

	/* Setting a bit takes an erase, and the data behind goes back with it. */
	zassert_ok(flash_area_erase(flash_area, block, UBI_TEST_PEB_SIZE));
	zassert_ok(flash_area_write(flash_area, block, block_scratch,
				    sizeof(block_scratch)));

	flash_area_close(flash_area);
}

uint32_t corrupt_volume_tables(psa_key_id_t ikm_key_id, uint32_t copies)
{
	const struct flash_area *flash_area = NULL;
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_vid_header vid = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };
	uint32_t damaged = 0;

	zassert_ok(ubi_impl_key_derive(ikm_key_id, &key_header,
				       &key_volume_table));
	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	const uint8_t blank = flash_area_erased_val(flash_area);

	for (uint32_t pnum = 0; pnum < UBI_TEST_PEB_COUNT && damaged < copies;
	     ++pnum) {
		const off_t block = (off_t)pnum * UBI_TEST_PEB_SIZE;

		zassert_ok(flash_area_read(flash_area,
					   block + UBI_VID_HEADER_OFFSET,
					   buffer, sizeof(buffer)));

		/* The sealed header says which block holds a copy. */
		if (UBI_HEADER_OK !=
		    ubi_impl_header_vid_parse(buffer, sizeof(buffer),
					      key_header, pnum, blank, &vid))
			continue;

		if (UBI_VOLUME_TABLE_VOL_ID != vid.vol_id)
			continue;

		damaged +=
			corrupt_a_byte_at(flash_area, block + UBI_DATA_OFFSET,
					  UBI_TEST_PEB_SIZE - UBI_DATA_OFFSET);
	}

	flash_area_close(flash_area);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	return damaged;
}

uint32_t corrupt_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t damaged = 0;
	off_t at = UBI_DATA_OFFSET;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	while (0 <= (at = data_find(flash_area, needle, length, at))) {
		damaged += corrupt_a_byte_at(flash_area, at, length);
		at += UBI_TEST_PEB_SIZE;
	}

	flash_area_close(flash_area);

	return damaged;
}

uint32_t corrupt_header_of_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t damaged = 0;
	off_t at = UBI_DATA_OFFSET;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	while (0 <= (at = data_find(flash_area, needle, length, at))) {
		const off_t vid = at - UBI_DATA_OFFSET + UBI_VID_HEADER_OFFSET;

		/* The checksum covers the whole header, so any byte will do. */
		damaged += corrupt_a_byte_at(flash_area, vid, UBI_HEADER_SIZE);
		at += UBI_TEST_PEB_SIZE;
	}

	flash_area_close(flash_area);

	return damaged;
}

uint32_t count_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint32_t found = 0;
	off_t at = UBI_DATA_OFFSET;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	while (0 <= (at = data_find(flash_area, needle, length, at))) {
		found += 1;
		at += UBI_TEST_PEB_SIZE;
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

#if defined(CONFIG_FLASH_SIMULATOR)

void flash_fail_writes_after(uint32_t after)
{
	writes_left = after;
	writes_fail = true;
	faults_apply();
}

void flash_fail_writes_never(void)
{
	writes_fail = false;
	faults_apply();
}

void flash_fail_erases_after(uint32_t after)
{
	erases_left = after;
	erases_fail = true;
	faults_apply();
}

void flash_fail_erases_never(void)
{
	erases_fail = false;
	faults_apply();
}

uint32_t flash_ops(const char *name)
{
	struct stats_hdr *group = stats_group_find("flash_sim_stats");
	struct counter_wanted wanted = { .name = name,
					 .value = 0,
					 .found = false };

	zassert_not_null(group, "the flash simulator keeps no counters");
	zassert_ok(stats_walk(group, counter_match, &wanted));
	zassert_true(wanted.found, "there is no counter called \"%s\"", name);

	return wanted.value;
}

uint32_t flash_erases_of(uint32_t pnum)
{
	const struct flash_area *flash_area = NULL;
	char name[COUNTER_NAME_SIZE] = { 0 };

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	const uint32_t unit =
		(uint32_t)(flash_area->fa_off / UBI_TEST_PEB_SIZE) + pnum;

	flash_area_close(flash_area);

	const int length =
		snprintf(name, sizeof(name), "erase_cycles_unit%u", unit);

	zassert_true(0 < length);
	zassert_true((size_t)length < sizeof(name));

	return flash_ops(name);
}

void flash_ops_forget(void)
{
	struct stats_hdr *group = stats_group_find("flash_sim_stats");

	zassert_not_null(group, "the flash simulator keeps no counters");
	stats_reset(group);
}

#endif /* CONFIG_FLASH_SIMULATOR */
