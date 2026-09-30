/**
 * \file    test_kat.c
 * \author  Kamil Kielbasa
 * \brief   Known answers: what reaches the flash, byte for byte.
 *
 *          The expected bytes come from scripts/kat.py, which follows
 *          docs/on-flash-format.md and shares no code with the library. A
 *          change that alters the key derivation or any byte of a header or
 *          a record fails here, even when writing and reading still agree
 *          with each other.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <string.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_key.h"
#include "ubi_volume_table.h"

/* Test headers: */
#include "headers.h"
#include "keys.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Image every known answer belongs to. */
#define KAT_IMAGE_SEQ (0x12345678UL)

/* Module variables and constants ------------------------------------------ */

/** Context the bound keys below are derived under. */
static const uint8_t kat_key_context[] = "storage_partition";

/* python3 scripts/kat.py */

static const uint8_t kat_header_key_fingerprint[16] = {
	0xF9, 0x9A, 0xD0, 0xB9, 0x3D, 0x3E, 0x66, 0xE8,
	0xEF, 0x83, 0x0E, 0xE9, 0x6B, 0x75, 0x67, 0x6B,
};

static const uint8_t kat_volume_table_key_fingerprint[16] = {
	0xA4, 0x7F, 0xD5, 0x8F, 0x57, 0x81, 0x24, 0x7E,
	0x2B, 0xEE, 0x5E, 0xC4, 0x74, 0xF6, 0x08, 0x16,
};

static const uint8_t kat_ec_header[64] = {
	0x55, 0x42, 0x49, 0x23, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
	0x00, 0x80, 0x12, 0x34, 0x56, 0x78, 0x7C, 0x9A, 0x9A, 0xBD, 0x8A,
	0x25, 0xA6, 0xD7, 0xE8, 0x5E, 0xCE, 0x81, 0x84, 0x60, 0x5B, 0x86,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xDF, 0xE6, 0x8F,
};

static const uint8_t kat_vid_header[64] = {
	0x55, 0x42, 0x49, 0x21, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x09, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00,
	0x01, 0x00, 0xCE, 0x62, 0xBB, 0x88, 0xB1, 0xC8, 0x8B, 0x28, 0xB8,
	0xAF, 0x59, 0xEE, 0xEF, 0x51, 0xF9, 0x67, 0x01, 0x02, 0x03, 0x04,
	0x05, 0x06, 0x07, 0x08, 0xCA, 0xFE, 0xF0, 0x0D, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x98, 0xF8, 0x1C,
};

static const uint8_t kat_volume_table_record[96] = {
	0x55, 0x42, 0x49, 0x56, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
	0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x10, 0x6C, 0x6F, 0x67, 0x73, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x05, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x00, 0x95, 0xA4, 0xD5, 0xF8,
	0xFB, 0xEA, 0x33, 0xB9, 0xB6, 0xB2, 0x82, 0x89, 0xDB, 0x40, 0x7D, 0xFB,
};

static const uint8_t kat_bound_header_key_fingerprint[16] = {
	0x25, 0xE8, 0x16, 0x40, 0xC1, 0x6F, 0x96, 0xFB,
	0x2D, 0xC7, 0x63, 0xB1, 0xF5, 0x10, 0x7B, 0x17,
};

static const uint8_t kat_bound_volume_table_key_fingerprint[16] = {
	0x0A, 0x4E, 0x00, 0xE0, 0x39, 0x46, 0xBC, 0x23,
	0xEF, 0xD5, 0xD5, 0x62, 0xA7, 0x9E, 0x23, 0x80,
};

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_unit, test_both_keys_are_the_ones_the_format_specifies)
{
	uint8_t fingerprint[UBI_MAC_SIZE] = { 0 };

	key_fingerprint(key_header, fingerprint, sizeof(fingerprint));
	zassert_mem_equal(kat_header_key_fingerprint, fingerprint,
			  sizeof(fingerprint));

	key_fingerprint(key_volume_table, fingerprint, sizeof(fingerprint));
	zassert_mem_equal(kat_volume_table_key_fingerprint, fingerprint,
			  sizeof(fingerprint));
}

ZTEST(ubi_unit, test_keys_under_a_context_are_the_ones_the_format_specifies)
{
	psa_key_id_t bound_header = PSA_KEY_ID_NULL;
	psa_key_id_t bound_volume_table = PSA_KEY_ID_NULL;
	uint8_t fingerprint[UBI_MAC_SIZE] = { 0 };

	zassert_ok(ubi_impl_key_derive(ikm_key, kat_key_context,
				       sizeof(kat_key_context) - 1,
				       &bound_header, &bound_volume_table));

	key_fingerprint(bound_header, fingerprint, sizeof(fingerprint));
	zassert_mem_equal(kat_bound_header_key_fingerprint, fingerprint,
			  sizeof(fingerprint));

	key_fingerprint(bound_volume_table, fingerprint, sizeof(fingerprint));
	zassert_mem_equal(kat_bound_volume_table_key_fingerprint, fingerprint,
			  sizeof(fingerprint));

	ubi_impl_key_destroy(&bound_header);
	ubi_impl_key_destroy(&bound_volume_table);
}

ZTEST(ubi_unit, test_an_ec_header_is_sealed_as_the_format_specifies)
{
	const struct ubi_ec_header header = {
		.erase_count = 1,
		.image_seq = KAT_IMAGE_SEQ,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_ec_serialize(&header, key_header, TEST_PNUM,
						buffer, sizeof(buffer)));
	zassert_mem_equal(kat_ec_header, buffer, sizeof(buffer));
}

ZTEST(ubi_unit, test_a_vid_header_is_sealed_as_the_format_specifies)
{
	const struct ubi_vid_header header = {
		.sqnum = 0x0102030405060708ULL,
		.vol_id = 3,
		.lnum = 9,
		.image_seq = KAT_IMAGE_SEQ,
		.data_size = 256,
		.data_crc = 0xCAFEF00DUL,
		.copy_flag = true,
	};
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_vid_serialize(&header, key_header, TEST_PNUM,
						 buffer, sizeof(buffer)));
	zassert_mem_equal(kat_vid_header, buffer, sizeof(buffer));
}

ZTEST(ubi_unit, test_a_record_is_sealed_as_the_format_specifies)
{
	const struct ubi_volume_table_record record = {
		.revision = 7,
		.image_seq = KAT_IMAGE_SEQ,
		.peb_size = 4096,
		.peb_count = 2048,
		.vol_id_watermark = 3,
		.volume_count = 2,
		.entries = {
			{ .vol_id = 0, .leb_count = 16, .name = "logs" },
			{ .vol_id = 2, .leb_count = 5,
			  .name = "0123456789abcde" },
		},
	};
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	size_t record_size = 0;

	zassert_ok(ubi_impl_volume_table_record_serialize(
		&record, key_volume_table, buffer, sizeof(buffer),
		&record_size));
	zassert_equal(sizeof(kat_volume_table_record), record_size);
	zassert_mem_equal(kat_volume_table_record, buffer, record_size);
}
