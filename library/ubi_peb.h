/**
 * \file    ubi_peb.h
 * \author  Kamil Kielbasa
 * \brief   Lifecycle of a physical erase block.
 *
 *          The state every block is in, the erase-and-stamp that makes a
 *          block usable, and the choice of which block to use next. Block
 *          numbers arriving here are within the partition.
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
 * \param pnum                          Physical erase block.
 *
 * \return Its state.
 */
enum ubi_peb_state ubi_impl_peb_state_get(const struct ubi_device *ubi,
					  uint32_t pnum);

/**
 * \brief Set the lifecycle state of a physical erase block.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Physical erase block.
 * \param state                         State to record.
 */
void ubi_impl_peb_state_set(struct ubi_device *ubi, uint32_t pnum,
			    enum ubi_peb_state state);

/**
 * \brief Erase a block and stamp it with a fresh erase counter header.
 *
 *        The count comes from RAM, or from the header while formatting. With
 *        \c CONFIG_UBI_ERASE_INVALIDATES_HEADERS every header that still
 *        verifies is cleared first, so an erase cut short is recognised. A
 *        block left unerased leaves an attached device read-only.
 *
 * \param[in,out] ubi                   Device holding the partition.
 * \param pnum                          Physical erase block to prepare.
 *
 * \retval 0
 *         Erased and stamped; the block is #UBI_PEB_FREE.
 * \retval -EINVAL
 *         The block has been erased #UBI_MAX_ERASE_COUNT times already.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_peb_prepare(struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Erase and stamp a block waiting for reclaim, blank or foreign, and
 *        retire it if it refuses.
 *
 *        A block an unmap let go of goes together with every other copy of
 *        its logical block still waiting.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Block to reclaim.
 *
 * \retval 0
 *         Erased and stamped; the block is #UBI_PEB_FREE.
 * \retval -EINVAL
 *         A block has been erased #UBI_MAX_ERASE_COUNT times already.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_peb_reclaim(struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Choose a block to write next and hand it over ready to use.
 *
 *        Takes the most worn free block within
 *        \c CONFIG_UBI_WEAR_LEVELING_THRESHOLD erases of the least worn one.
 *        With none free, erases the least worn blank block, and failing that
 *        the least worn one waiting for reclaim.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[out] pnum                     Block to use, left #UBI_PEB_FREE.
 *
 * \retval 0
 *         Allocated.
 * \retval -ENOSPC
 *         Every block is in use or out of service.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_peb_allocate_for_write(struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Choose a free block for relocation to move data onto: the most
 *        worn one within twice the levelling threshold of the least worn.
 *
 * \param[in] ubi                       Attached device.
 * \param[out] pnum                     Block to use, left #UBI_PEB_FREE.
 *
 * \retval 0
 *         Allocated.
 * \retval -ENOSPC
 *         No block is free.
 */
int ubi_impl_peb_allocate_for_levelling(const struct ubi_device *ubi,
					uint32_t *pnum);

/**
 * \brief Retire a block that would not take a write or an erase, in RAM
 *        only, so that a reattach gives it another chance.
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
 * \brief Erase a block whose header must not stand, and retire it.
 *
 *        An erase that fails leaves the device read-only, and is logged: the
 *        next attach may take what the block holds.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Block to withdraw.
 * \param vol_id                        Volume its header names.
 * \param lnum                          Logical block its header names.
 */
void ubi_impl_peb_withdraw(struct ubi_device *ubi, uint32_t pnum,
			   uint32_t vol_id, uint32_t lnum);

/**
 * \brief Take a retired block out of service until the next attach, after it
 *        failed its second chance.
 *
 * \param[in,out] ubi                   Attached device.
 * \param pnum                          Block to write off.
 */
void ubi_impl_peb_write_off(struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Erase every block waiting for reclaim that still names one of the
 *        logical blocks \p first to \p first + \p count - 1 of a volume.
 *
 *        Such a block is an older copy, or the one an unmap let go of, and
 *        the next attach would hand it back. The copies a change or an
 *        attach turned down go before the one an unmap let go of, each the
 *        oldest first, so an erase cut short leaves only the newest.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param first                         First logical block concerned.
 * \param count                         How many.
 *
 * \retval 0
 *         No such copy is left.
 * \retval -EIO
 *         A block could not be read or erased; it may still name one.
 */
int ubi_impl_peb_purge(struct ubi_device *ubi, uint32_t vol_id, uint32_t first,
		       uint32_t count);

#endif /* UBI_PEB_H */
