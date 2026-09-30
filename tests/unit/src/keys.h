/**
 * \file    keys.h
 * \author  Kamil Kielbasa
 * \brief   Keying material and key helpers the unit tests share.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef KEYS_H
#define KEYS_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* PSA headers: */
#include <psa/crypto.h>

/* Variable declarations --------------------------------------------------- */

/** Keying material every derivation in the suite starts from. */
extern const uint8_t test_ikm[32];

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

#endif /* KEYS_H */
