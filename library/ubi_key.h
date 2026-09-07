/**
 * \file    ubi_key.h
 * \author  Kamil Kielbasa
 * \brief   Derivation of the keys UBI authenticates its metadata with.
 *
 *          The application supplies a PSA handle to its input keying
 *          material and UBI derives two keys from it with HKDF-SHA256: one
 *          for the block headers, one for the volume table record. Splitting
 *          them costs nothing and keeps the two domains apart, so a volume
 *          table record can never pass as a block header.
 *
 *          The image sequence number salts both derivations, which means a
 *          reformat produces different keys and headers from the previous
 *          image cannot verify even if their image sequence number were
 *          somehow forced to match.
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
 * 128-bit tag rather than recovering the key.
 */
#define UBI_KEY_BITS (128)

BUILD_ASSERT(128 == UBI_KEY_BITS || 192 == UBI_KEY_BITS || 256 == UBI_KEY_BITS,
	     "UBI_KEY_BITS must be a length AES accepts");

BUILD_ASSERT(0 != PSA_MAC_LENGTH(PSA_KEY_TYPE_AES, UBI_KEY_BITS, PSA_ALG_CMAC),
	     "PSA does not recognise AES-CMAC at UBI_KEY_BITS");

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Derive the block header and volume table keys.
 *
 *        On failure neither output is left holding a live key.
 *
 * \param ikm_key_id                    Application key handle. Must carry
 *                                      \c PSA_KEY_USAGE_DERIVE and permit
 *                                      \c PSA_ALG_HKDF(PSA_ALG_SHA_256).
 * \param image_seq                     Salts both derivations.
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
int ubi_key_derive(psa_key_id_t ikm_key_id, uint32_t image_seq,
		   psa_key_id_t *key_header, psa_key_id_t *key_volume_table);

/**
 * \brief Destroy a derived key and clear the handle.
 *
 *        Destroying a handle that holds no key is a no-op, so this may be
 *        called on a partially initialised device.
 *
 * \param[in,out] key_id                Handle to destroy.
 */
void ubi_key_destroy(psa_key_id_t *key_id);

#endif /* UBI_KEY_H */
