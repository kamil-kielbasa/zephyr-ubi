/**
 * \file    test_key.c
 * \author  Kamil Kielbasa
 * \brief   Deriving the two keys from one piece of keying material.
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
