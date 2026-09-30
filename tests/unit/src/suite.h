/**
 * \file    suite.h
 * \author  Kamil Kielbasa
 * \brief   The keys the suite derives once and every test leans on.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef SUITE_H
#define SUITE_H

/* Include files ----------------------------------------------------------- */

/* PSA headers: */
#include <psa/crypto.h>

/* Variable declarations --------------------------------------------------- */

/** The PSA key holding the suite's keying material. */
extern psa_key_id_t ikm_key;

/** Key the headers are authenticated with, derived from it. */
extern psa_key_id_t key_header;

/** Key the volume table record is authenticated with. */
extern psa_key_id_t key_volume_table;

#endif /* SUITE_H */
