/**
 * \file    ubi_peb.h
 * \author  Kamil Kielbasa
 * \brief   Lifecycle of a physical erase block.
 *
 *          Which blocks exist is the partition's business, but what each one
 *          is currently for belongs here: the state every block is in, the
 *          erase-and-stamp that turns an unknown block into a usable one, and
 *          the choice of which block to use next.
 *
 *          Internal to the library. Arguments arriving here have already
 *          passed the boundary in ubi_api.c, so a block number outside the
 *          partition is a defect rather than an input to report on.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_PEB_H
#define UBI_PEB_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* UBI headers: */
#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Read the lifecycle state of a physical erase block.
 *
 * \param[in] ubi                       Attached device.
 * \param pnum                          Physical erase block, within range.
 *
 * \return Its state.
 */
enum ubi_peb_state ubi_peb_state_get(const struct ubi_device *ubi,
				     uint32_t pnum);

/**
 * \brief Set it. The narrowing to a byte happens here and nowhere else.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Physical erase block, within range.
 * \param state                         State to record.
 */
void ubi_peb_state_set(struct ubi_device *ubi, uint32_t pnum,
		       enum ubi_peb_state state);

/**
 * \brief Erase a block and stamp it with a fresh erase counter header.
 *
 *        Wear history is worth keeping, so the previous count is read back
 *        first. A block whose count can no longer be read starts again from
 *        the average of the blocks that still have one, not from zero: zero
 *        would make it look brand new and \ref ubi_peb_allocate would keep
 *        choosing it until it wore out.
 *
 * \param[in,out] ubi                   Device holding the partition.
 * \param pnum                          Physical erase block to prepare.
 * \param[out] history_lost             Set when the count had to be guessed.
 *
 * \retval 0
 *         Erased and stamped; the block is now #UBI_PEB_FREE.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_peb_prepare(struct ubi_device *ubi, uint32_t pnum, bool *history_lost);

/**
 * \brief Choose a block to write next and hand it over ready to use.
 *
 *        Prefers a block that is already erased and stamped, and among those
 *        the least worn one, so that allocation spreads wear on its own.
 *        Falls back to erasing a block whose contents are unknown. Blocks
 *        waiting for #UBI_MAINTENANCE_RECLAIM are left alone: they still hold
 *        data, and reclaiming is the application's call to make.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[out] pnum                     Block to use, left #UBI_PEB_FREE.
 *
 * \retval 0
 *         Allocated.
 * \retval -ENOSPC
 *         No block is available; reclaiming may free some.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_peb_allocate(struct ubi_device *ubi, uint32_t *pnum);

#endif /* UBI_PEB_H */
