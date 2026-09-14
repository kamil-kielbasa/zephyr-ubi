/**
 * \file    ubi_leb.h
 * \author  Kamil Kielbasa
 * \brief   What a logical erase block can be asked to do.
 *
 *          A LEB is an address; a PEB is where it currently lives. Every
 *          operation here is about that relationship: making one, breaking
 *          it, moving it somewhere fresh, or reading and writing through it.
 *
 *          Internal to the library: the boundary in ubi_api.c has already
 *          checked what arrives here.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_LEB_H
#define UBI_LEB_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Put a physical block behind a logical one, leaving it empty.
 *
 *        A block that already has one keeps it.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 *
 * \retval 0
 *         Mapped, or already was.
 * \retval -EINVAL
 *         The volume does not reach that far.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         No physical block available.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Take the physical block away and queue it for reclaim.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 *
 * \retval 0
 *         Unmapped, or already was.
 * \retval -EINVAL
 *         The volume does not reach that far.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_impl_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Take the physical block away and erase it before returning.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 *
 * \retval 0
 *         Erased, or nothing was mapped.
 * \retval -EINVAL
 *         The volume does not reach that far.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EIO
 *         The crypto backend or the flash driver failed; the block is queued
 *         for reclaim so a later attempt can retry it.
 */
int ubi_impl_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Read through the mapping, or read erased bytes when there is none.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 * \param offset                        Byte offset within the block.
 * \param[out] buffer                   Destination.
 * \param length                        Bytes to read.
 *
 * \retval 0
 *         Read.
 * \retval -EINVAL
 *         The volume does not reach that far, or the range spills past the
 *         end of the block.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBADMSG
 *         Only with \c CONFIG_UBI_VERIFY_ON_READ: the header did not verify.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		      uint32_t offset, uint8_t *buffer, size_t length);

/**
 * \brief Replace what a logical block holds, atomically.
 *
 *        The new contents go to a block of their own and the mapping moves
 *        only once they are down, so an interruption leaves the old ones.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write, at most the block
 *                                      size.
 *
 * \retval 0
 *         The new contents are durable.
 * \retval -EINVAL
 *         The volume does not reach that far, or the data does not fit.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         No physical block available.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
			const uint8_t *buffer, size_t length);

/**
 * \brief Write where the caller says, and promise nothing else.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 * \param offset                        Byte offset within the block.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write.
 *
 * \retval 0
 *         Written.
 * \retval -EINVAL
 *         The volume does not reach that far, the range spills past the end
 *         of the block, or the alignment rule was broken.
 * \retval -ENOENT
 *         No such volume, or the block is not mapped.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_leb_write_at(struct ubi_device *ubi, uint32_t vol_id,
			  uint32_t lnum, uint32_t offset, const uint8_t *buffer,
			  size_t length);

/**
 * \brief Say whether a logical block has a physical one, and how worn it is.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 * \param[out] info                     Receives the state.
 *
 * \retval 0
 *         Described.
 * \retval -EINVAL
 *         The volume does not reach that far.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_impl_leb_get_info(struct ubi_device *ubi, uint32_t vol_id,
			  uint32_t lnum, struct ubi_leb_info *info);

#endif /* UBI_LEB_H */
