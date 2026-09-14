/**
 * \file    suite.c
 * \author  Kamil Kielbasa
 * \brief   The unit suite: one crypto init and one derivation for all of it.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_key.h"

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

psa_key_id_t ikm_key;
psa_key_id_t key_header;
psa_key_id_t key_volume_table;

/* Static function definitions --------------------------------------------- */

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
