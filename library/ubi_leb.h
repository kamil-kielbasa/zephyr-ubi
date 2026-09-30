/**
 * \file    ubi_leb.h
 * \author  Kamil Kielbasa
 * \brief   What a logical erase block can be asked to do.
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
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 *
 * \retval 0
 *         Mapped.
 * \retval -EINVAL
 *         The volume does not reach that far.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EEXIST
 *         A physical block backs it already.
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
 * \brief Take the physical block away and erase it before returning, along
 *        with every older copy still waiting for reclaim.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 *
 * \retval 0
 *         No copy of the block is left on the flash.
 * \retval -EINVAL
 *         The volume does not reach that far.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EIO
 *         The crypto backend or the flash driver failed. A block that could
 *         not be erased is retired and leaves the device read-only; what it
 *         held may come back after a reboot.
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
 * \param length                        Bytes to read; zero reads nothing.
 *
 * \retval 0
 *         Read.
 * \retval -EINVAL
 *         The volume does not reach that far, or the range spills past the
 *         end of the block.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBADMSG
 *         Only with \c CONFIG_UBI_VERIFY_ON_READ: the header did not verify,
 *         or it names another logical block or another image.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		      uint32_t offset, uint8_t *buffer, size_t length);

/**
 * \brief Replace what a logical block holds, atomically.
 *
 *        The new contents go to a block of their own and the mapping moves
 *        only once they are down, so an interruption leaves the old ones,
 *        or what reached the flash when there were none.
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
 *        An unmapped block is mapped on the way.
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
 *         No such volume.
 * \retval -ENOSPC
 *         The block was unmapped and no physical block was available.
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
