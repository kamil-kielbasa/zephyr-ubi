/**
 * \file    ubi_key.h
 * \author  Kamil Kielbasa
 * \brief   Derivation of the keys UBI authenticates its metadata with.
 *
 *          The application supplies a PSA handle to its input keying
 *          material and UBI derives two keys from it with HKDF-SHA256: one
 *          for the block headers, one for the volume table record, so that a
 *          record can never pass as a header.
 *
 *          The image sequence number plays no part here; it lives inside the
 *          authenticated headers instead. That is what lets attach build its
 *          keys before reading a single byte of flash.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_KEY_H
#define UBI_KEY_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/toolchain.h>

/* PSA headers: */
#include <psa/crypto.h>

/* Defines ----------------------------------------------------------------- */

/**
 * Length of the derived keys, in bits.
 *
 * PSA cannot supply this: key length is a policy choice, not a property of
 * the algorithm. AES-128 is ample, because forging a header means forging a
 * 128-bit MAC rather than recovering the key.
 */
#define UBI_KEY_BITS (128)

/**
 * Length of the AES-CMAC written into every header and into the volume table
 * record.
 *
 * Written out rather than taken from PSA on purpose: this is an on-flash
 * dimension, so a change in the crypto backend must not move it silently.
 * The assert below is what ties the two together.
 */
#define UBI_MAC_SIZE (16)

BUILD_ASSERT(128 == UBI_KEY_BITS || 192 == UBI_KEY_BITS || 256 == UBI_KEY_BITS,
	     "UBI_KEY_BITS must be a length AES accepts");

BUILD_ASSERT(0 != PSA_MAC_LENGTH(PSA_KEY_TYPE_AES, UBI_KEY_BITS, PSA_ALG_CMAC),
	     "PSA does not recognise AES-CMAC at UBI_KEY_BITS");

BUILD_ASSERT(UBI_MAC_SIZE == PSA_MAC_LENGTH(PSA_KEY_TYPE_AES, UBI_KEY_BITS,
					    PSA_ALG_CMAC),
	     "on-flash MAC size must match what AES-CMAC produces");

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Derive the block header and volume table keys.
 *
 *        On failure neither output is left holding a live key.
 *
 * \param ikm_key_id                    Application key handle. Must carry
 *                                      \c PSA_KEY_USAGE_DERIVE and permit
 *                                      \c PSA_ALG_HKDF(PSA_ALG_SHA_256).
 * \param[out] key_header               Key authenticating EC and VID headers.
 * \param[out] key_volume_table         Key authenticating the volume table
 *                                      record.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         An output pointer is \c NULL, or \p ikm_key_id is
 *         \c PSA_KEY_ID_NULL.
 * \retval -EACCES
 *         \p ikm_key_id does not exist, or its policy forbids this
 *         derivation.
 * \retval -EIO
 *         The crypto backend failed.
 */
int ubi_impl_key_derive(psa_key_id_t ikm_key_id, psa_key_id_t *key_header,
			psa_key_id_t *key_volume_table);

/**
 * \brief Destroy a derived key and clear the handle.
 *
 *        Destroying a handle that holds no key is a no-op, so this may be
 *        called on a partially initialised device.
 *
 * \param[in,out] key_id                Handle to destroy.
 */
void ubi_impl_key_destroy(psa_key_id_t *key_id);

#endif /* UBI_KEY_H */
