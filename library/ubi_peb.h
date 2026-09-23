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
enum ubi_peb_state ubi_impl_peb_state_get(const struct ubi_device *ubi,
					  uint32_t pnum);

/**
 * \brief Set it. The narrowing to a byte happens here and nowhere else.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Physical erase block, within range.
 * \param state                         State to record.
 */
void ubi_impl_peb_state_set(struct ubi_device *ubi, uint32_t pnum,
			    enum ubi_peb_state state);

/**
 * \brief Erase a block and stamp it with a fresh erase counter header.
 *
 *        Wear history is worth keeping, so the previous count is read back
 *        first. A block whose count can no longer be read starts again from
 *        the average of the blocks that still have one, not from zero: zero
 *        would make it look brand new and \ref ubi_impl_peb_allocate_for_write would keep
 *        choosing it until it wore out.
 *
 * \param[in,out] ubi                   Device holding the partition.
 * \param pnum                          Physical erase block to prepare.
 *
 * \retval 0
 *         Erased and stamped; the block is now #UBI_PEB_FREE.
 * \retval -EINVAL
 *         The block has been erased #UBI_MAX_ERASE_COUNT times already.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_peb_prepare(struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Choose a block to write next and hand it over ready to use.
 *
 *        Prefers a block that is already erased and stamped, taking the most
 *        worn one still within \c CONFIG_UBI_WEAR_LEVELING_THRESHOLD erases
 *        of the least worn. Falls back to erasing an #UBI_PEB_UNKNOWN block.
 *        Blocks waiting for #UBI_MAINTENANCE_RECLAIM are left alone.
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
int ubi_impl_peb_allocate_for_write(struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Choose a well worn block that is ready to use.
 *
 *        The counterpart of \ref ubi_impl_peb_allocate_for_write: levelling moves data
 *        that never changes onto a block already erased many times. The reach
 *        is twice as far but still bounded, so relocation cannot keep landing
 *        on the single most worn block. Only #UBI_PEB_FREE blocks qualify.
 *
 * \param[in] ubi                       Attached device.
 * \param[out] pnum                     Block to use, left #UBI_PEB_FREE.
 *
 * \retval 0
 *         Allocated.
 * \retval -ENOSPC
 *         No block is erased and waiting; reclaiming may free some.
 */
int ubi_impl_peb_allocate_for_levelling(const struct ubi_device *ubi,
					uint32_t *pnum);

/**
 * \brief Retire a block that would not take a write or an erase.
 *
 *        Held in RAM only, so a reattach gives the block another chance. A
 *        persistent fault will retire it again; a one-off will not condemn
 *        it forever.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Block to retire.
 * \param vol_id                        Volume it was serving, or
 *                                      #UBI_VOL_ID_INVALID.
 * \param lnum                          Logical block it was serving.
 */
void ubi_impl_peb_retire(struct ubi_device *ubi, uint32_t pnum, uint32_t vol_id,
			 uint32_t lnum);

/**
 * \brief Write a block off after it refused a second time.
 *
 *        \ref ubi_impl_peb_retire leaves a block in line for another attempt;
 *        this is what that attempt reaches when it fails. Out of service
 *        until the next attach, and no longer counted as work waiting, so
 *        that a block which is finished stops costing an erase every time
 *        the application asks for repairs.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Block to write off.
 */
void ubi_impl_peb_write_off(struct ubi_device *ubi, uint32_t pnum);

#endif /* UBI_PEB_H */
