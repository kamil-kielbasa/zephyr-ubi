/**
 * \file    common.h
 * \author  Kamil Kielbasa
 * \brief   Fixed inputs and the few helpers the unit tests share.
 *
 *          Neither header serialization nor key derivation has a public API
 *          of its own, so the tests reach them through the library's internal
 *          headers. Nothing is added to the library to make it testable.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef COMMON_H
#define COMMON_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_header.h"

/* Defines ----------------------------------------------------------------- */

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

/* Variable declarations --------------------------------------------------- */

/** Keying material every derivation in the suite starts from. */
extern const uint8_t test_ikm[32];

/** A block straight out of an erase. */
extern const uint8_t erased_block[UBI_HEADER_SIZE];

/* Function declarations --------------------------------------------------- */

/**
 * \brief Import keying material as a PSA derivation key.
 */
psa_key_id_t import_ikm(const uint8_t *ikm, size_t length,
			psa_key_usage_t usage);

/**
 * \brief Fingerprint a derived key by authenticating a fixed message with it.
 *
 *        Derived keys are not exportable, so this is how two of them are
 *        compared for equality.
 */
void key_fingerprint(psa_key_id_t key_id, uint8_t *MAC, size_t mac_size);

/**
 * \brief Recompute the CRC so that only the MAC can flag a change.
 */
void fix_crc(uint8_t *buffer);

#endif /* COMMON_H */
