/**
 * \file    test_volume_table.c
 * \author  Kamil Kielbasa
 * \brief   The volume table record: what it accepts and what it refuses.
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
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_key.h"
#include "ubi_volume_table.h"

/* Test headers: */
#include "headers.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/* Where docs/on-flash-format.md puts the fields these tests touch. */
#define RECORD_VERSION_OFFSET (0x04)
#define RECORD_VOLUME_COUNT_OFFSET (0x1C)
#define RECORD_FIRST_NAME_OFFSET (0x28)
#define RECORD_NAME_SIZE (16)

/* Module variables and constants ------------------------------------------ */

static const struct ubi_volume_table_record sample = {
	.revision = 4,
	.image_seq = TEST_IMAGE_SEQ,
	.peb_size = 4096,
	.peb_count = 64,
	.vol_id_watermark = 2,
	.volume_count = 2,
	.entries = {
		{ .vol_id = 0, .leb_count = 3, .name = "config" },
		{ .vol_id = 1, .leb_count = 8, .name = "logs" },
	},
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Serialize \ref sample into \p buffer and return its length.
 */
static size_t sample_serialize(uint8_t *buffer, size_t buffer_size);

/**
 * \brief Seal \p buffer again after a change, as a key holder could.
 */
static void record_reseal(uint8_t *buffer, size_t record_size);

/* Static function definitions --------------------------------------------- */

static size_t sample_serialize(uint8_t *buffer, size_t buffer_size)
{
	size_t record_size = 0;

	zassert_ok(ubi_impl_volume_table_record_serialize(
		&sample, key_volume_table, buffer, buffer_size, &record_size));

	return record_size;
}

static void record_reseal(uint8_t *buffer, size_t record_size)
{
	const size_t mac_offset = record_size - UBI_MAC_SIZE;
	size_t mac_length = 0;

	zassert_equal(PSA_SUCCESS,
		      psa_mac_compute(key_volume_table, PSA_ALG_CMAC, buffer,
				      mac_offset, &buffer[mac_offset],
				      UBI_MAC_SIZE, &mac_length));
}

/* Module interface function definitions ----------------------------------- */

ZTEST(ubi_unit, test_a_record_reads_back_as_it_was_written)
{
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	struct ubi_volume_table_record record = { 0 };
	const size_t record_size = sample_serialize(buffer, sizeof(buffer));

	zassert_equal(UBI_HEADER_OK,
		      ubi_impl_volume_table_record_parse(
			      buffer, record_size, key_volume_table, &record));
	zassert_equal(sample.revision, record.revision);
	zassert_equal(sample.image_seq, record.image_seq);
	zassert_equal(sample.peb_size, record.peb_size);
	zassert_equal(sample.peb_count, record.peb_count);
	zassert_equal(sample.vol_id_watermark, record.vol_id_watermark);
	zassert_equal(sample.volume_count, record.volume_count);

	for (uint32_t i = 0; i < sample.volume_count; ++i) {
		zassert_equal(sample.entries[i].vol_id,
			      record.entries[i].vol_id);
		zassert_equal(sample.entries[i].leb_count,
			      record.entries[i].leb_count);
		zassert_str_equal(sample.entries[i].name,
				  record.entries[i].name);
	}
}

ZTEST(ubi_unit, test_a_record_is_refused_whatever_byte_changes)
{
	uint8_t original[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	const size_t record_size = sample_serialize(original, sizeof(original));

	for (size_t i = 0; i < record_size; ++i) {
		uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
		struct ubi_volume_table_record record = { 0 };
		enum ubi_header_status expected = UBI_HEADER_TAMPERED;

		memcpy(buffer, original, sizeof(buffer));
		buffer[i] ^= 0x01;

		const uint32_t count =
			sys_get_be32(&buffer[RECORD_VOLUME_COUNT_OFFSET]);

		/* A foreign magic is not ours to judge; a count this build
		 * cannot hold is refused before the MAC it would size. */
		if (i < sizeof(uint32_t)) {
			expected = UBI_HEADER_NOT_UBI;
		} else if (CONFIG_UBI_MAX_NR_OF_VOLUMES < count) {
			expected = UBI_HEADER_UNSUPPORTED;
		}

		zassert_equal(expected,
			      ubi_impl_volume_table_record_parse(
				      buffer, sizeof(buffer), key_volume_table,
				      &record),
			      "byte %zu escaped detection", i);
	}
}

ZTEST(ubi_unit, test_a_record_of_another_version_is_unsupported)
{
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	struct ubi_volume_table_record record = { 0 };
	const size_t record_size = sample_serialize(buffer, sizeof(buffer));

	/* Authentic, but written by a build that speaks another format. */
	buffer[RECORD_VERSION_OFFSET] += 1;
	record_reseal(buffer, record_size);

	zassert_equal(UBI_HEADER_UNSUPPORTED,
		      ubi_impl_volume_table_record_parse(
			      buffer, record_size, key_volume_table, &record));
}

ZTEST(ubi_unit, test_a_record_with_more_volumes_than_the_build_is_unsupported)
{
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	struct ubi_volume_table_record record = { 0 };

	zassert_true(0 < sample_serialize(buffer, sizeof(buffer)));
	sys_put_be32(CONFIG_UBI_MAX_NR_OF_VOLUMES + 1,
		     &buffer[RECORD_VOLUME_COUNT_OFFSET]);

	zassert_equal(UBI_HEADER_UNSUPPORTED,
		      ubi_impl_volume_table_record_parse(buffer, sizeof(buffer),
							 key_volume_table,
							 &record));
}

ZTEST(ubi_unit, test_a_record_longer_than_its_buffer_is_corrupt)
{
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	struct ubi_volume_table_record record = { 0 };
	const size_t record_size = sample_serialize(buffer, sizeof(buffer));

	zassert_equal(UBI_HEADER_CORRUPT, ubi_impl_volume_table_record_parse(
						  buffer, record_size - 1,
						  key_volume_table, &record));
}

ZTEST(ubi_unit, test_a_name_filling_its_field_is_read_whole)
{
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	char full[RECORD_NAME_SIZE] = { 0 };
	struct ubi_volume_table_record record = { 0 };
	const size_t record_size = sample_serialize(buffer, sizeof(buffer));

	BUILD_ASSERT(RECORD_NAME_SIZE == UBI_VOLUME_NAME_MAX_LEN);

	/* No terminator fits the field, so the reader has to add its own. */
	memset(full, 'A', sizeof(full));
	memcpy(&buffer[RECORD_FIRST_NAME_OFFSET], full, sizeof(full));
	record_reseal(buffer, record_size);

	zassert_equal(UBI_HEADER_OK,
		      ubi_impl_volume_table_record_parse(
			      buffer, record_size, key_volume_table, &record));
	zassert_equal(UBI_VOLUME_NAME_MAX_LEN, strlen(record.entries[0].name));
	zassert_mem_equal(full, record.entries[0].name, sizeof(full));
}

ZTEST(ubi_unit, test_a_record_is_not_written_past_its_buffer)
{
	uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE] = { 0 };
	struct ubi_volume_table_record record = sample;
	size_t record_size = 0;

	zassert_equal(-EINVAL,
		      ubi_impl_volume_table_record_serialize(
			      &sample, key_volume_table, buffer,
			      UBI_VOLUME_TABLE_PREAMBLE_SIZE, &record_size));

	record.volume_count = CONFIG_UBI_MAX_NR_OF_VOLUMES + 1;
	zassert_equal(-EINVAL, ubi_impl_volume_table_record_serialize(
				       &record, key_volume_table, buffer,
				       sizeof(buffer), &record_size));
}
