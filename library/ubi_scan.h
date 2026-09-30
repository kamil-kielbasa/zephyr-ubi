/**
 * \file    ubi_scan.h
 * \author  Kamil Kielbasa
 * \brief   The two passes attach makes over the partition.
 *
 *          The first judges every block by its headers alone and finds the
 *          volume table copies. The second, once the volume table is known,
 *          hangs every block of this image off the logical block it names.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_SCAN_H
#define UBI_SCAN_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* UBI headers: */
#include "ubi_private.h"

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief What the first pass counted, for when no volume table turns up and
 *        attach has to say why.
 */
struct ubi_scan {
	/** Headers that are well formed but do not verify under this key. */
	uint32_t unopenable;
	/** Headers that verify but were written by another release. */
	uint32_t unsupported;
	/** Blocks holding a logical block of some volume. */
	uint32_t claimed;
};

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Classify every block, record the erase counts and the highest
 *        sequence number, and find the block holding each volume table copy.
 *
 * \param[in,out] ubi                   Device being attached.
 * \param[out] scan                     What was counted on the way.
 *
 * \retval 0
 *         Every block was classified.
 * \retval -EIO
 *         A block could not be read, or the crypto backend failed.
 */
int ubi_impl_scan_first_pass(struct ubi_device *ubi, struct ubi_scan *scan);

/**
 * \brief Drop the blocks of other images and hang the rest off the logical
 *        blocks they name, the newer of two copies winning when its data
 *        matches its seal.
 *
 * \param[in,out] ubi                   Device being attached, its volumes
 *                                      built from the volume table.
 *
 * \retval 0
 *         Every block was placed.
 * \retval -EIO
 *         A block could not be read, or the crypto backend failed.
 */
int ubi_impl_scan_second_pass(struct ubi_device *ubi);

#endif /* UBI_SCAN_H */
