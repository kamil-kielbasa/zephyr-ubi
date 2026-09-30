/**
 * \file    forge.h
 * \author  Kamil Kielbasa
 * \brief   Headers rewritten on the flash, authentic or not, as a test needs
 *          them.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef FORGE_H
#define FORGE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>

/* PSA headers: */
#include <psa/crypto.h>

/* Function declarations --------------------------------------------------- */

/**
 * \brief Erase a block and give it an authentic erase counter header.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param pnum                          Block to restamp.
 * \param image_seq                     Image the block claims.
 * \param erase_count                   Count to write into the header.
 */
void stamp_erase_count(psa_key_id_t ikm_key_id, uint32_t pnum,
		       uint32_t image_seq, uint64_t erase_count);

/**
 * \brief Give a block a new authentic erase counter header and keep what
 *        follows it.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param pnum                          Block to restamp.
 * \param image_seq                     Image the block claims.
 * \param erase_count                   Count to write into the header.
 */
void erase_count_rewrite(psa_key_id_t ikm_key_id, uint32_t pnum,
			 uint32_t image_seq, uint64_t erase_count);

/**
 * \brief Read the erase counter header of every block straight off the flash.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param[out] counts                   One count per block.
 * \param peb_count                     How many blocks to read.
 */
void erase_counts_on_flash(psa_key_id_t ikm_key_id, uint64_t *counts,
			   uint32_t peb_count);

/**
 * \brief Change one byte of a header and repair its checksum, so only the
 *        MAC can object.
 *
 * \param pnum                          Block to rewrite.
 * \param at                            Offset of the byte within the block.
 */
void forge_header_byte(uint32_t pnum, off_t at);

/**
 * \brief Point a block's volume identifier header at another volume and
 *        logical block, authenticated as the library would have done it.
 */
void vid_rewrite(psa_key_id_t ikm_key_id, uint32_t pnum, uint32_t vol_id,
		 uint32_t lnum);

/**
 * \brief Make a block's volume identifier header name another image,
 *        authenticated as the library would have done it.
 */
void vid_image_rewrite(psa_key_id_t ikm_key_id, uint32_t pnum,
		       uint32_t image_seq);

#endif /* FORGE_H */
