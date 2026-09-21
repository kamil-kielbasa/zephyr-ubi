/**
 * \file    common.c
 * \author  Kamil Kielbasa
 * \brief   Fixed inputs and the few helpers the unit tests share.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

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

/* Module variables and constants ------------------------------------------ */

const uint8_t test_ikm[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
	0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

const uint8_t erased_block[UBI_HEADER_SIZE] = {
	[0 ...(UBI_HEADER_SIZE - 1)] = 0xFF,
};

/* Module interface function definitions ----------------------------------- */

psa_key_id_t import_ikm(const uint8_t *ikm, size_t length,
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

void key_fingerprint(psa_key_id_t key_id, uint8_t *MAC, size_t mac_size)
{
	static const uint8_t message[] = "fingerprint";
	size_t mac_length = 0;

	zassert_true(UBI_MAC_SIZE <= mac_size);

	zassert_equal(PSA_SUCCESS, psa_mac_compute(key_id, PSA_ALG_CMAC,
						   message, sizeof(message) - 1,
						   MAC, mac_size, &mac_length));
	zassert_equal(UBI_MAC_SIZE, mac_length);
}

void fix_crc(uint8_t *buffer)
{
	sys_put_be32(crc32_ieee(buffer, HEADER_CRC_OFFSET),
		     &buffer[HEADER_CRC_OFFSET]);
}
