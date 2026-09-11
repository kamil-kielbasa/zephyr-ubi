/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   Unit tests for header serialization and key derivation.
 *
 *          Neither module has a public API of its own, so the tests reach
 *          them through the library's internal headers. Nothing is added to
 *          the library to make it testable.
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

/* Module defines ---------------------------------------------------------- */

/** Offset of the CRC within either header. */
#define HEADER_CRC_OFFSET (0x3C)

/** A byte inside the erase counter field, safe to flip in tests. */
#define EC_ERASE_COUNT_OFFSET (0x08)

/** Arbitrary but fixed block number for tests that do not vary it. */
#define TEST_PNUM (7)

/** Arbitrary but fixed image sequence number. */
#define TEST_IMAGE_SEQ (0xA5A5F00DUL)

/** Byte an erase leaves behind on the flash these tests pretend to use. */
#define TEST_ERASE_VALUE (0xFF)

/* Module variables and constants ------------------------------------------ */

static const uint8_t test_ikm[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
	0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

/** A block straight out of an erase. */
static const uint8_t erased_block[UBI_HEADER_SIZE] = {
	[0 ...(UBI_HEADER_SIZE - 1)] = 0xFF,
};

static psa_key_id_t ikm_key;
static psa_key_id_t key_header;
static psa_key_id_t key_volume_table;

/* Static function definitions --------------------------------------------- */

/**
 * \brief Import the test keying material as a PSA derivation key.
 */
static psa_key_id_t import_ikm(const uint8_t *ikm, size_t length,
			       psa_key_usage_t usage)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key_id = PSA_KEY_ID_NULL;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attributes, usage);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);

	zassert_equal(PSA_SUCCESS,
		      psa_import_key(&attributes, ikm, length, &key_id));

	return key_id;
}

/**
 * \brief Fingerprint a derived key by tagging a fixed message with it.
 *
 *        Derived keys are not exportable, so this is how two of them are
 *        compared for equality.
 */
static void key_fingerprint(psa_key_id_t key_id, uint8_t *tag, size_t tag_size)
{
	static const uint8_t message[] = "fingerprint";
	size_t tag_length = 0;

	zassert_true(UBI_HEADER_TAG_SIZE <= tag_size);

	zassert_equal(PSA_SUCCESS, psa_mac_compute(key_id, PSA_ALG_CMAC,
						   message, sizeof(message) - 1,
						   tag, tag_size, &tag_length));
	zassert_equal(UBI_HEADER_TAG_SIZE, tag_length);
}

/**
 * \brief Recompute the CRC so that only the tag can flag a change.
 */
static void fix_crc(uint8_t *buffer)
{
	sys_put_be32(crc32_ieee(buffer, HEADER_CRC_OFFSET),
		     &buffer[HEADER_CRC_OFFSET]);
}

static void *suite_setup(void)
{
	zassert_equal(PSA_SUCCESS, psa_crypto_init());

	ikm_key = import_ikm(test_ikm, sizeof(test_ikm), PSA_KEY_USAGE_DERIVE);

	zassert_ok(ubi_key_derive(ikm_key, &key_header, &key_volume_table));

	return NULL;
}

static void suite_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	ubi_key_destroy(&key_header);
	ubi_key_destroy(&key_volume_table);
	ubi_key_destroy(&ikm_key);
}

ZTEST_SUITE(ubi_unit, NULL, suite_setup, NULL, NULL, suite_teardown);

/* Tests: serialization round trip ----------------------------------------- */

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

	zassert_ok(ubi_ec_header_serialize(&written, key_header, TEST_PNUM,
					   buffer, sizeof(buffer)));
	zassert_equal(UBI_HEADER_OK,
		      ubi_ec_header_parse(buffer, sizeof(buffer), key_header,
					  TEST_PNUM, TEST_ERASE_VALUE, &read));

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

	zassert_ok(ubi_vid_header_serialize(&written, key_header, TEST_PNUM,
					    buffer, sizeof(buffer)));
	zassert_equal(UBI_HEADER_OK,
		      ubi_vid_header_parse(buffer, sizeof(buffer), key_header,
					   TEST_PNUM, TEST_ERASE_VALUE, &read));

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
		      ubi_ec_header_parse(erased_block, sizeof(erased_block),
					  key_header, TEST_PNUM,
					  TEST_ERASE_VALUE, NULL));
	zassert_equal(UBI_HEADER_ERASED,
		      ubi_vid_header_parse(erased_block, sizeof(erased_block),
					   key_header, TEST_PNUM,
					   TEST_ERASE_VALUE, NULL));
}

ZTEST(ubi_unit, test_blankness_follows_the_flash_erase_value)
{
	uint8_t zeroed_block[UBI_HEADER_SIZE] = { 0 };

	/* A flash that erases to zero must not be told that its blank blocks
	 * hold something, nor that a block of 0xFF is blank. */
	zassert_equal(UBI_HEADER_ERASED,
		      ubi_ec_header_parse(zeroed_block, sizeof(zeroed_block),
					  key_header, TEST_PNUM, 0x00, NULL));
	zassert_not_equal(UBI_HEADER_ERASED,
			  ubi_ec_header_parse(erased_block,
					      sizeof(erased_block), key_header,
					      TEST_PNUM, 0x00, NULL));
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

	zassert_ok(ubi_ec_header_serialize(&header, key_header, TEST_PNUM,
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

	zassert_ok(ubi_vid_header_serialize(&header, key_header, TEST_PNUM,
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

	zassert_ok(ubi_ec_header_serialize(&header, key_header, TEST_PNUM,
					   buffer, sizeof(buffer)));

	/* The tag verifies, but this build cannot address that layout. */
	zassert_equal(UBI_HEADER_NOT_UBI,
		      ubi_ec_header_parse(buffer, sizeof(buffer), key_header,
					  TEST_PNUM, TEST_ERASE_VALUE, NULL));
}

/* Tests: damage versus tampering ------------------------------------------ */

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

/* Tests: key derivation --------------------------------------------------- */

ZTEST(ubi_unit, test_derivation_is_deterministic)
{
	psa_key_id_t again_header = PSA_KEY_ID_NULL;
	psa_key_id_t again_volume_table = PSA_KEY_ID_NULL;
	uint8_t first[UBI_HEADER_TAG_SIZE] = { 0 };
	uint8_t second[UBI_HEADER_TAG_SIZE] = { 0 };

	zassert_ok(ubi_key_derive(ikm_key, &again_header, &again_volume_table));

	key_fingerprint(key_header, first, sizeof(first));
	key_fingerprint(again_header, second, sizeof(second));
	zassert_mem_equal(first, second, sizeof(first));

	key_fingerprint(key_volume_table, first, sizeof(first));
	key_fingerprint(again_volume_table, second, sizeof(second));
	zassert_mem_equal(first, second, sizeof(first));

	ubi_key_destroy(&again_header);
	ubi_key_destroy(&again_volume_table);
}

ZTEST(ubi_unit, test_derivation_separates_its_two_labels)
{
	uint8_t of_header[UBI_HEADER_TAG_SIZE] = { 0 };
	uint8_t of_volume_table[UBI_HEADER_TAG_SIZE] = { 0 };

	key_fingerprint(key_header, of_header, sizeof(of_header));
	key_fingerprint(key_volume_table, of_volume_table,
			sizeof(of_volume_table));

	zassert_true(0 !=
		     memcmp(of_header, of_volume_table, sizeof(of_header)));
}

ZTEST(ubi_unit, test_derivation_follows_the_key_material)
{
	uint8_t other_ikm[sizeof(test_ikm)] = { 0 };
	psa_key_id_t other_key = PSA_KEY_ID_NULL;
	psa_key_id_t other_header = PSA_KEY_ID_NULL;
	psa_key_id_t other_volume_table = PSA_KEY_ID_NULL;
	uint8_t original[UBI_HEADER_TAG_SIZE] = { 0 };
	uint8_t derived[UBI_HEADER_TAG_SIZE] = { 0 };

	memcpy(other_ikm, test_ikm, sizeof(other_ikm));
	other_ikm[0] ^= 0x01;
	other_key =
		import_ikm(other_ikm, sizeof(other_ikm), PSA_KEY_USAGE_DERIVE);

	zassert_ok(
		ubi_key_derive(other_key, &other_header, &other_volume_table));

	key_fingerprint(key_header, original, sizeof(original));
	key_fingerprint(other_header, derived, sizeof(derived));

	/* One bit of input keying material must change everything. */
	zassert_true(0 != memcmp(original, derived, sizeof(original)));

	ubi_key_destroy(&other_header);
	ubi_key_destroy(&other_volume_table);
	ubi_key_destroy(&other_key);
}

ZTEST(ubi_unit, test_derivation_rejects_a_key_it_may_not_use)
{
	psa_key_id_t wrong =
		import_ikm(test_ikm, sizeof(test_ikm), PSA_KEY_USAGE_EXPORT);
	psa_key_id_t derived_header = PSA_KEY_ID_NULL;
	psa_key_id_t derived_volume_table = PSA_KEY_ID_NULL;

	zassert_equal(-EACCES, ubi_key_derive(wrong, &derived_header,
					      &derived_volume_table));
	zassert_equal(PSA_KEY_ID_NULL, derived_header);
	zassert_equal(PSA_KEY_ID_NULL, derived_volume_table);

	ubi_key_destroy(&wrong);
}

ZTEST(ubi_unit, test_derivation_rejects_an_absent_key)
{
	psa_key_id_t derived_header = PSA_KEY_ID_NULL;
	psa_key_id_t derived_volume_table = PSA_KEY_ID_NULL;

	zassert_equal(-EINVAL, ubi_key_derive(PSA_KEY_ID_NULL, &derived_header,
					      &derived_volume_table));
	zassert_equal(-EINVAL,
		      ubi_key_derive(ikm_key, NULL, &derived_volume_table));
}
