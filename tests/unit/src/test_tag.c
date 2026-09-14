/**
 * \file    test_tag.c
 * \author  Kamil Kielbasa
 * \brief   What the tag covers, and what it refuses to pass.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_key.h"

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_unit, test_crc_reports_damage)
{
	const struct ubi_ec_header header = { .erase_count = 1 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_ec_header_serialize(&header, key_header, TEST_PNUM,
					   buffer, sizeof(buffer)));

	/* A byte flips and nobody fixes the checksum: that is damage. */
	buffer[EC_ERASE_COUNT_OFFSET] ^= 0x01;

	zassert_equal(UBI_HEADER_CORRUPT,
		      ubi_ec_header_parse(buffer, sizeof(buffer), key_header,
					  TEST_PNUM, TEST_ERASE_VALUE, NULL));
}

ZTEST(ubi_unit, test_tag_reports_tampering_of_every_byte)
{
	const struct ubi_ec_header header = {
		.erase_count = 99,
		.image_seq = TEST_IMAGE_SEQ,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};
	uint8_t original[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_ec_header_serialize(&header, key_header, TEST_PNUM,
					   original, sizeof(original)));

	/*
	 * Flip one bit at a time across everything the CRC covers, repairing
	 * the CRC each round so that only the tag can object. Magic is checked
	 * before the tag, so those four bytes report a foreign block instead.
	 */
	for (size_t i = 0; i < HEADER_CRC_OFFSET; ++i) {
		const enum ubi_header_status expected =
			(i < sizeof(uint32_t)) ? UBI_HEADER_NOT_UBI :
						 UBI_HEADER_TAMPERED;
		uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

		memcpy(buffer, original, sizeof(buffer));
		buffer[i] ^= 0x01;
		fix_crc(buffer);

		zassert_equal(expected,
			      ubi_ec_header_parse(buffer, sizeof(buffer),
						  key_header, TEST_PNUM,
						  TEST_ERASE_VALUE, NULL),
			      "byte %zu escaped detection", i);
	}
}

/* Tests: what the tag binds ----------------------------------------------- */

ZTEST(ubi_unit, test_tag_binds_the_block_number)
{
	const struct ubi_ec_header header = { .erase_count = 5 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_ec_header_serialize(&header, key_header, TEST_PNUM,
					   buffer, sizeof(buffer)));

	/* Byte-for-byte identical, read from a different block: rejected. */
	zassert_equal(UBI_HEADER_TAMPERED,
		      ubi_ec_header_parse(buffer, sizeof(buffer), key_header,
					  TEST_PNUM + 1, TEST_ERASE_VALUE,
					  NULL));
}

ZTEST(ubi_unit, test_header_and_volume_table_keys_are_separate)
{
	const struct ubi_ec_header header = { .erase_count = 5 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_ec_header_serialize(&header, key_volume_table, TEST_PNUM,
					   buffer, sizeof(buffer)));

	zassert_equal(UBI_HEADER_TAMPERED,
		      ubi_ec_header_parse(buffer, sizeof(buffer), key_header,
					  TEST_PNUM, TEST_ERASE_VALUE, NULL));
}

ZTEST(ubi_unit, test_vid_tag_does_not_pass_as_ec)
{
	const struct ubi_vid_header header = { .vol_id = 1, .lnum = 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_vid_header_serialize(&header, key_header, TEST_PNUM,
					    buffer, sizeof(buffer)));

	/* The magic sits inside the authenticated input, so the two header
	 * kinds can never be confused for one another. */
	zassert_equal(UBI_HEADER_NOT_UBI,
		      ubi_ec_header_parse(buffer, sizeof(buffer), key_header,
					  TEST_PNUM, TEST_ERASE_VALUE, NULL));
}

/* Tests: the argument contract -------------------------------------------- */

ZTEST(ubi_unit, test_short_buffer_is_refused)
{
	const struct ubi_ec_header header = { .erase_count = 1 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_equal(-EINVAL,
		      ubi_ec_header_serialize(&header, key_header, TEST_PNUM,
					      buffer, UBI_HEADER_SIZE - 1));
	zassert_equal(-EINVAL,
		      ubi_vid_header_serialize(NULL, key_header, TEST_PNUM,
					       buffer, sizeof(buffer)));
	zassert_equal(UBI_HEADER_ERROR,
		      ubi_ec_header_parse(buffer, UBI_HEADER_SIZE - 1,
					  key_header, TEST_PNUM,
					  TEST_ERASE_VALUE, NULL));
	zassert_equal(UBI_HEADER_ERROR,
		      ubi_vid_header_parse(NULL, sizeof(buffer), key_header,
					   TEST_PNUM, TEST_ERASE_VALUE, NULL));
}

ZTEST(ubi_unit, test_absent_key_is_refused)
{
	const struct ubi_ec_header header = { .erase_count = 1 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_equal(-EINVAL, ubi_ec_header_serialize(&header, PSA_KEY_ID_NULL,
						       TEST_PNUM, buffer,
						       sizeof(buffer)));
	zassert_equal(UBI_HEADER_ERROR,
		      ubi_vid_header_parse(buffer, sizeof(buffer),
					   PSA_KEY_ID_NULL, TEST_PNUM,
					   TEST_ERASE_VALUE, NULL));
}
