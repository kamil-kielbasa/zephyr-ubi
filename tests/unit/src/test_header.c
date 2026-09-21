/**
 * \file    test_header.c
 * \author  Kamil Kielbasa
 * \brief   Serializing a header, parsing it back, and the layout it pins.
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

ZTEST(ubi_unit, test_ec_header_roundtrip)
{
	const struct ubi_ec_header written = {
		.erase_count = 0x0123456789ABCDEFULL,
		.image_seq = TEST_IMAGE_SEQ,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};
	struct ubi_ec_header read = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_ec_serialize(&written, key_header, TEST_PNUM,
						buffer, sizeof(buffer)));
	zassert_equal(UBI_HEADER_OK,
		      ubi_impl_header_ec_parse(buffer, sizeof(buffer),
					       key_header, TEST_PNUM,
					       TEST_ERASE_VALUE, &read));

	zassert_equal(written.erase_count, read.erase_count);
	zassert_equal(written.image_seq, read.image_seq);
	zassert_equal(written.vid_header_offset, read.vid_header_offset);
	zassert_equal(written.data_offset, read.data_offset);
}

ZTEST(ubi_unit, test_vid_header_roundtrip)
{
	const struct ubi_vid_header written = {
		.sqnum = 0xFEDCBA9876543210ULL,
		.vol_id = 3,
		.lnum = 41,
		.image_seq = TEST_IMAGE_SEQ,
		.data_size = 1234,
		.data_crc = 0xDEADBEEFUL,
		.copy_flag = true,
	};
	struct ubi_vid_header read = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_vid_serialize(
		&written, key_header, TEST_PNUM, buffer, sizeof(buffer)));
	zassert_equal(UBI_HEADER_OK,
		      ubi_impl_header_vid_parse(buffer, sizeof(buffer),
						key_header, TEST_PNUM,
						TEST_ERASE_VALUE, &read));

	zassert_equal(written.sqnum, read.sqnum);
	zassert_equal(written.vol_id, read.vol_id);
	zassert_equal(written.lnum, read.lnum);
	zassert_equal(written.image_seq, read.image_seq);
	zassert_equal(written.data_size, read.data_size);
	zassert_equal(written.data_crc, read.data_crc);
	zassert_true(read.copy_flag);
}

ZTEST(ubi_unit, test_erased_block_has_no_header)
{
	zassert_equal(UBI_HEADER_ERASED,
		      ubi_impl_header_ec_parse(
			      erased_block, sizeof(erased_block), key_header,
			      TEST_PNUM, TEST_ERASE_VALUE, NULL));
	zassert_equal(UBI_HEADER_ERASED,
		      ubi_impl_header_vid_parse(
			      erased_block, sizeof(erased_block), key_header,
			      TEST_PNUM, TEST_ERASE_VALUE, NULL));
}

ZTEST(ubi_unit, test_blankness_follows_the_flash_erase_value)
{
	uint8_t zeroed_block[UBI_HEADER_SIZE] = { 0 };

	/* A flash that erases to zero must not be told that its blank blocks
	 * hold something, nor that a block of 0xFF is blank. */
	zassert_equal(UBI_HEADER_ERASED,
		      ubi_impl_header_ec_parse(zeroed_block,
					       sizeof(zeroed_block), key_header,
					       TEST_PNUM, 0x00, NULL));
	zassert_not_equal(
		UBI_HEADER_ERASED,
		ubi_impl_header_ec_parse(erased_block, sizeof(erased_block),
					 key_header, TEST_PNUM, 0x00, NULL));
}

/* Tests: the on-flash layout is a contract -------------------------------- */

ZTEST(ubi_unit, test_ec_layout_is_pinned)
{
	const struct ubi_ec_header header = {
		.erase_count = 0x0011223344556677ULL,
		.image_seq = 0x8899AABBUL,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_ec_serialize(&header, key_header, TEST_PNUM,
						buffer, sizeof(buffer)));

	/* Every field Linux UBI interprets sits where Linux puts it. */
	zassert_equal(0x55424923UL, sys_get_be32(&buffer[0x00]), "magic");
	zassert_equal(1, buffer[0x04], "version");
	zassert_equal(header.erase_count, sys_get_be64(&buffer[0x08]), "ec");
	zassert_equal(UBI_VID_HEADER_OFFSET, sys_get_be32(&buffer[0x10]));
	zassert_equal(UBI_DATA_OFFSET, sys_get_be32(&buffer[0x14]));
	zassert_equal(header.image_seq, sys_get_be32(&buffer[0x18]));
	zassert_equal(crc32_ieee(buffer, 0x3C), sys_get_be32(&buffer[0x3C]));
}

ZTEST(ubi_unit, test_vid_layout_is_pinned)
{
	const struct ubi_vid_header header = {
		.sqnum = 0x0011223344556677ULL,
		.vol_id = 0x01020304UL,
		.lnum = 0x05060708UL,
		.image_seq = 0x8899AABBUL,
		.data_size = 0x0A0B0C0DUL,
		.data_crc = 0x0E0F1011UL,
		.copy_flag = true,
	};
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_vid_serialize(&header, key_header, TEST_PNUM,
						 buffer, sizeof(buffer)));

	zassert_equal(0x55424921UL, sys_get_be32(&buffer[0x00]), "magic");
	zassert_equal(1, buffer[0x04], "version");
	zassert_equal(1, buffer[0x05], "vol_type is always dynamic");
	zassert_equal(1, buffer[0x06], "copy_flag");
	zassert_equal(0, buffer[0x07], "compat");
	zassert_equal(header.vol_id, sys_get_be32(&buffer[0x08]));
	zassert_equal(header.lnum, sys_get_be32(&buffer[0x0C]));
	zassert_equal(header.image_seq, sys_get_be32(&buffer[0x10]));
	zassert_equal(header.data_size, sys_get_be32(&buffer[0x14]));
	zassert_equal(header.sqnum, sys_get_be64(&buffer[0x28]));
	zassert_equal(header.data_crc, sys_get_be32(&buffer[0x30]));
	zassert_equal(crc32_ieee(buffer, 0x3C), sys_get_be32(&buffer[0x3C]));
}

ZTEST(ubi_unit, test_authentic_header_with_foreign_layout_is_refused)
{
	const struct ubi_ec_header header = {
		.erase_count = 1,
		.image_seq = TEST_IMAGE_SEQ,
		.vid_header_offset = 512,
		.data_offset = 1024,
	};
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_header_ec_serialize(&header, key_header, TEST_PNUM,
						buffer, sizeof(buffer)));

	/* The MAC verifies, but this build cannot address that layout. */
	zassert_equal(UBI_HEADER_NOT_UBI,
		      ubi_impl_header_ec_parse(buffer, sizeof(buffer),
					       key_header, TEST_PNUM,
					       TEST_ERASE_VALUE, NULL));
}
