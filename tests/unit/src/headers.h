/**
 * \file    headers.h
 * \author  Kamil Kielbasa
 * \brief   Fixed header inputs and the byte-level helper the unit tests
 *          share.
 *
 *          Header serialization has no public API, so the tests reach it
 *          through the library's internal headers.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef HEADERS_H
#define HEADERS_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

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

/** A block straight out of an erase. */
extern const uint8_t erased_block[UBI_HEADER_SIZE];

/* Function declarations --------------------------------------------------- */

/**
 * \brief Recompute the CRC so that only the MAC can flag a change.
 */
void fix_crc(uint8_t *buffer);

#endif /* HEADERS_H */
