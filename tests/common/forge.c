/**
 * \file    forge.c
 * \author  Kamil Kielbasa
 * \brief   Headers rewritten on the flash, authentic or not, as a test needs
 *          them.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_key.h"

/* Test headers: */
#include "forge.h"
#include "partition.h"

/* Module defines ---------------------------------------------------------- */

/** The checksum is the last word of either header. */
#define HEADER_CRC_OFFSET (UBI_HEADER_SIZE - sizeof(uint32_t))

/* Module interface function definitions ----------------------------------- */

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

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
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

void erase_count_rewrite(psa_key_id_t ikm_key_id, uint32_t pnum,
			 uint32_t image_seq, uint64_t erase_count)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;

	const struct ubi_ec_header header = {
		.erase_count = erase_count,
		.image_seq = image_seq,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};

	block_save(pnum, block_scratch);

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));
	zassert_ok(ubi_impl_header_ec_serialize(
		&header, key_header, pnum, block_scratch, UBI_HEADER_SIZE));

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	block_restore(pnum, block_scratch);
}

void erase_counts_on_flash(psa_key_id_t ikm_key_id, uint64_t *counts,
			   uint32_t peb_count)
{
	const struct flash_area *flash_area = NULL;
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_ec_header header = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_key_derive(ikm_key_id, NULL, 0, &key_header,
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

void vid_rewrite(psa_key_id_t ikm_key_id, uint32_t pnum, uint32_t vol_id,
		 uint32_t lnum)
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

	vid.vol_id = vol_id;
	vid.lnum = lnum;

	zassert_ok(ubi_impl_header_vid_serialize(&vid, key_header, pnum, header,
						 UBI_HEADER_SIZE));

	block_restore(pnum, block_scratch);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);
}

void vid_image_rewrite(psa_key_id_t ikm_key_id, uint32_t pnum,
		       uint32_t image_seq)
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

	vid.image_seq = image_seq;

	zassert_ok(ubi_impl_header_vid_serialize(&vid, key_header, pnum, header,
						 UBI_HEADER_SIZE));

	block_restore(pnum, block_scratch);

	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);
}
