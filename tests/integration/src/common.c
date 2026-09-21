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
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/stats/stats.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_header.h"
#include "ubi_key.h"

/* Test headers: */
#include "common.h"

/* Module defines ---------------------------------------------------------- */

/** Chunk used when sweeping the whole partition. */
#define SWEEP_CHUNK (256)

/* Module type definitions ------------------------------------------------- */

/** One counter being looked for while walking the statistics group. */
struct counter_wanted {
	/** Name to match. */
	const char *name;
	/** What it held, left at zero when no name matched. */
	uint32_t value;
};

/* Module variables and constants ------------------------------------------ */

/** Bytes the injected write fault still lets through. */
static uint32_t writes_left;

/* Static function declarations -------------------------------------------- */

/**
 * \brief Find the next block whose data area starts with \p needle.
 *
 * \return Its offset, or -1 when there is no such block left.
 */
static off_t data_find(const struct flash_area *flash_area,
		       const uint8_t *needle, size_t length, off_t from);

/**
 * \brief Stand in for a worn out cell: refuse the write once the allowance
 *        set by \ref flash_fail_writes_after has run out.
 */
static int write_byte_fails(const struct device *dev, off_t offset,
			    uint8_t data);

/**
 * \brief Pick the wanted counter out of a statistics group being walked.
 */
static int counter_match(struct stats_hdr *group, void *arg, const char *name,
			 uint16_t offset);

/* Static function definitions --------------------------------------------- */

static int counter_match(struct stats_hdr *group, void *arg, const char *name,
			 uint16_t offset)
{
	struct counter_wanted *wanted = arg;

	if (0 == strcmp(wanted->name, name))
		wanted->value = *(uint32_t *)((uint8_t *)group + offset);

	return 0;
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

static int write_byte_fails(const struct device *dev, off_t offset,
			    uint8_t data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(offset);

	if (0 == writes_left)
		return -EIO;

	writes_left -= 1;

	return data;
}

static const struct flash_simulator_cb failing_writes = {
	.write_byte = write_byte_fails,
};

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

void stamp_erase_count(psa_key_id_t ikm_key_id, uint32_t pnum,
		       uint32_t image_seq, uint64_t erase_count)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	uint8_t buffer[UBI_HEADER_SIZE];

	const struct ubi_ec_header header = {
		.erase_count = erase_count,
		.image_seq = image_seq,
		.vid_header_offset = UBI_TEST_VID_OFFSET,
		.data_offset = UBI_TEST_DATA_OFFSET,
	};

	zassert_ok(ubi_impl_key_derive(ikm_key_id, &key_header,
				       &key_volume_table));
	zassert_ok(ubi_impl_header_ec_serialize(&header, key_header, pnum,
						buffer, sizeof(buffer)));

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	const struct flash_area *flash_area = NULL;
	const off_t block = (off_t)pnum * UBI_TEST_PEB_SIZE;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));
	zassert_ok(flash_area_erase(flash_area, block, UBI_TEST_PEB_SIZE));
	zassert_ok(flash_area_write(flash_area, block, buffer, sizeof(buffer)));

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

uint32_t corrupt_header_of_data_matching(const uint8_t *needle, size_t length)
{
	const struct flash_area *flash_area = NULL;
	uint8_t header[UBI_TEST_DATA_OFFSET - UBI_TEST_VID_OFFSET];
	uint32_t damaged = 0;
	off_t at = UBI_TEST_DATA_OFFSET;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	while (0 <= (at = data_find(flash_area, needle, length, at))) {
		const off_t vid =
			at - UBI_TEST_DATA_OFFSET + UBI_TEST_VID_OFFSET;

		zassert_ok(flash_area_read(flash_area, vid, header,
					   sizeof(header)));

		/* Any byte with a bit left to clear will do: the checksum in
		 * front of the MAC covers the whole header. */
		for (size_t i = 0; i < sizeof(header); ++i) {
			if (0x00 == header[i])
				continue;

			flash_clear_a_bit(flash_area, vid + (off_t)i);
			damaged += 1;
			break;
		}

		at += UBI_TEST_PEB_SIZE;
	}

	flash_area_close(flash_area);

	return damaged;
}

void flash_fail_writes_after(uint32_t after)
{
	const struct flash_area *flash_area = NULL;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	writes_left = after;
	flash_simulator_set_callbacks(flash_area_get_device(flash_area),
				      &failing_writes);

	flash_area_close(flash_area);
}

void flash_fail_writes_never(void)
{
	const struct flash_area *flash_area = NULL;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	flash_simulator_set_callbacks(flash_area_get_device(flash_area), NULL);

	flash_area_close(flash_area);
}

uint32_t flash_ops(const char *name)
{
	struct stats_hdr *group = stats_group_find("flash_sim_stats");
	struct counter_wanted wanted = { .name = name, .value = 0 };

	zassert_not_null(group, "the flash simulator keeps no counters");
	zassert_ok(stats_walk(group, counter_match, &wanted));

	return wanted.value;
}

void flash_ops_forget(void)
{
	struct stats_hdr *group = stats_group_find("flash_sim_stats");

	zassert_not_null(group, "the flash simulator keeps no counters");
	stats_reset(group);
}
