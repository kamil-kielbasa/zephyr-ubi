/**
 * \file    table_copies.c
 * \author  Kamil Kielbasa
 * \brief   The volume table copies on the flash, found, read, damaged and
 *          forged behind the library's back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_key.h"
#include "ubi_volume_table.h"

/* Test headers: */
#include "partition.h"
#include "table_copies.h"

/* Module defines ---------------------------------------------------------- */

/** Copy number standing for every volume table copy. */
#define ANY_COPY (UINT32_MAX)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Copy the two headers of one block out.
 */
static void block_headers_read(uint32_t pnum, uint8_t *buffer);

/**
 * \brief Find the blocks carrying volume table copy \p lnum, or any copy for
 *        #ANY_COPY, in the order they sit in the partition.
 *
 * \return How many there are, which may be more than \p capacity.
 */
static uint32_t volume_table_scan(psa_key_id_t ikm_key_id, uint32_t lnum,
				  uint32_t *pnums, uint32_t capacity);

/**
 * \brief Report whether an attach could adopt the copy in block \p pnum.
 *
 * \param[out] revision                 Its revision, when it could.
 */
static bool volume_table_copy_adoptable(psa_key_id_t key_header,
					psa_key_id_t key_volume_table,
					uint32_t pnum, uint32_t *revision);

/* Module variables and constants ------------------------------------------ */

/** A volume table record decoded off the flash. */
static struct ubi_volume_table_record record_scratch = { 0 };

/* Static function definitions --------------------------------------------- */

static void block_headers_read(uint32_t pnum, uint8_t *buffer)
{
	const struct flash_area *flash_area = NULL;

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_read(flash_area, (off_t)pnum * UBI_TEST_PEB_SIZE,
				   buffer, UBI_DATA_OFFSET));
	flash_area_close(flash_area);
}

static uint32_t volume_table_scan(psa_key_id_t ikm_key_id, uint32_t lnum,
				  uint32_t *pnums, uint32_t capacity)
{
	const struct flash_area *flash_area = NULL;
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_vid_header vid = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };
	uint32_t found = 0;

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));
	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	for (uint32_t pnum = 0; pnum < UBI_TEST_PEB_COUNT; ++pnum) {
		zassert_ok(flash_area_read(flash_area,
					   (off_t)pnum * UBI_TEST_PEB_SIZE +
						   UBI_VID_HEADER_OFFSET,
					   buffer, sizeof(buffer)));

		const enum ubi_header_status status = ubi_impl_header_vid_parse(
			buffer, sizeof(buffer), key_header, pnum,
			UBI_TEST_ERASED, &vid);

		if (UBI_HEADER_OK != status ||
		    UBI_VOLUME_TABLE_VOL_ID != vid.vol_id)
			continue;

		if (ANY_COPY != lnum && lnum != vid.lnum)
			continue;

		if (found < capacity)
			pnums[found] = pnum;

		found += 1;
	}

	flash_area_close(flash_area);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	return found;
}

static bool volume_table_copy_adoptable(psa_key_id_t key_header,
					psa_key_id_t key_volume_table,
					uint32_t pnum, uint32_t *revision)
{
	const uint8_t *data = &block_scratch[UBI_DATA_OFFSET];
	struct ubi_ec_header ec = { 0 };
	struct ubi_vid_header vid = { 0 };

	/* The headers first: reading every block whole is slow. */
	block_headers_read(pnum, block_scratch);

	const enum ubi_header_status ec_status = ubi_impl_header_ec_parse(
		block_scratch, UBI_HEADER_SIZE, key_header, pnum,
		UBI_TEST_ERASED, &ec);

	if (UBI_HEADER_OK != ec_status)
		return false;

	const enum ubi_header_status vid_status = ubi_impl_header_vid_parse(
		&block_scratch[UBI_VID_HEADER_OFFSET], UBI_HEADER_SIZE,
		key_header, pnum, UBI_TEST_ERASED, &vid);

	if (UBI_HEADER_OK != vid_status ||
	    UBI_VOLUME_TABLE_VOL_ID != vid.vol_id ||
	    UBI_TEST_PEB_SIZE - UBI_DATA_OFFSET < vid.data_size)
		return false;

	block_save(pnum, block_scratch);

	const uint32_t crc = crc32_ieee(data, vid.data_size);

	if (vid.data_crc != crc)
		return false;

	const enum ubi_header_status record_status =
		ubi_impl_volume_table_record_parse(
			data, UBI_VOLUME_TABLE_DATA_MAX_SIZE, key_volume_table,
			&record_scratch);

	if (UBI_HEADER_OK != record_status)
		return false;

	*revision = record_scratch.revision;

	return true;
}

/* Module interface function definitions ----------------------------------- */

uint32_t corrupt_volume_tables(psa_key_id_t ikm_key_id, uint32_t copies)
{
	const struct flash_area *flash_area = NULL;
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_vid_header vid = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };
	uint32_t damaged = 0;

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
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
		const enum ubi_header_status status = ubi_impl_header_vid_parse(
			buffer, sizeof(buffer), key_header, pnum, blank, &vid);

		if (UBI_HEADER_OK != status ||
		    UBI_VOLUME_TABLE_VOL_ID != vid.vol_id)
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

uint32_t volume_table_blocks(psa_key_id_t ikm_key_id, uint32_t *pnums,
			     uint32_t capacity)
{
	return volume_table_scan(ikm_key_id, ANY_COPY, pnums, capacity);
}

void volume_table_copy_blocks(psa_key_id_t ikm_key_id,
			      uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT])
{
	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum) {
		zassert_equal(
			1, volume_table_scan(ikm_key_id, lnum, &pnums[lnum], 1),
			"volume table copy %u is not in exactly one block",
			lnum);
	}
}

size_t volume_table_record_read(psa_key_id_t ikm_key_id, uint32_t pnum,
				uint8_t *record, size_t capacity)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_vid_header vid = { 0 };

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));

	block_save(pnum, block_scratch);

	zassert_equal(
		UBI_HEADER_OK,
		ubi_impl_header_vid_parse(&block_scratch[UBI_VID_HEADER_OFFSET],
					  UBI_HEADER_SIZE, key_header, pnum,
					  UBI_TEST_ERASED, &vid),
		"block %u carries no volume identifier header", pnum);
	zassert_equal(UBI_VOLUME_TABLE_VOL_ID, vid.vol_id);
	zassert_true(vid.data_size <= capacity);

	memcpy(record, &block_scratch[UBI_DATA_OFFSET], vid.data_size);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	return vid.data_size;
}

void volume_table_reseal(psa_key_id_t ikm_key_id, uint32_t pnum,
			 uint32_t data_size)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_vid_header vid = { 0 };
	uint8_t *header = &block_scratch[UBI_VID_HEADER_OFFSET];

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));

	block_save(pnum, block_scratch);

	zassert_equal(UBI_HEADER_OK,
		      ubi_impl_header_vid_parse(header, UBI_HEADER_SIZE,
						key_header, pnum,
						UBI_TEST_ERASED, &vid));
	zassert_equal(UBI_VOLUME_TABLE_VOL_ID, vid.vol_id);

	vid.data_size = data_size;
	vid.data_crc = crc32_ieee(&block_scratch[UBI_DATA_OFFSET], data_size);
	vid.copy_flag = true;

	zassert_ok(ubi_impl_header_vid_serialize(&vid, key_header, pnum, header,
						 UBI_HEADER_SIZE));

	block_restore(pnum, block_scratch);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);
}

void volume_table_record_forge(psa_key_id_t ikm_key_id, uint32_t pnum,
			       uint8_t *record, size_t length)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_vid_header vid = { 0 };
	uint8_t *header = &block_scratch[UBI_VID_HEADER_OFFSET];
	uint8_t *data = &block_scratch[UBI_DATA_OFFSET];
	size_t mac_length = 0;

	zassert_true(UBI_MAC_SIZE < length);
	zassert_true(UBI_DATA_OFFSET + length <= sizeof(block_scratch));

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));
	zassert_equal(PSA_SUCCESS,
		      psa_mac_compute(key_volume_table, PSA_ALG_CMAC, record,
				      length - UBI_MAC_SIZE,
				      &record[length - UBI_MAC_SIZE],
				      UBI_MAC_SIZE, &mac_length));

	block_save(pnum, block_scratch);

	zassert_equal(UBI_HEADER_OK,
		      ubi_impl_header_vid_parse(header, UBI_HEADER_SIZE,
						key_header, pnum,
						UBI_TEST_ERASED, &vid));
	zassert_equal(UBI_VOLUME_TABLE_VOL_ID, vid.vol_id);

	/* What the old record held past the new one reads as never written. */
	memset(data, UBI_TEST_ERASED, sizeof(block_scratch) - UBI_DATA_OFFSET);
	memcpy(data, record, length);

	vid.data_size = (uint32_t)length;
	vid.data_crc = crc32_ieee(data, length);
	vid.copy_flag = true;

	zassert_ok(ubi_impl_header_vid_serialize(&vid, key_header, pnum, header,
						 UBI_HEADER_SIZE));

	block_restore(pnum, block_scratch);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);
}

uint32_t volume_table_adoptable(psa_key_id_t ikm_key_id, uint32_t *pnums,
				uint32_t *revisions, uint32_t capacity)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	uint32_t found = 0;

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));

	for (uint32_t pnum = 0; pnum < UBI_TEST_PEB_COUNT; ++pnum) {
		uint32_t revision = 0;
		const bool adoptable = volume_table_copy_adoptable(
			key_header, key_volume_table, pnum, &revision);

		if (!adoptable)
			continue;

		if (found < capacity && NULL != pnums)
			pnums[found] = pnum;

		if (found < capacity && NULL != revisions)
			revisions[found] = revision;

		found += 1;
	}

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	return found;
}
