/**
 * \file    ubi.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) public API.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_H
#define UBI_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* UBI headers: */
#include <ubi/types.h>

/* Module interface function declarations ---------------------------------- */

/* Device lifecycle */

/**
 * \brief Size in bytes of a UBI device handle.
 *
 * \return Size in bytes of \ref ubi_device.
 */
size_t ubi_device_size(void);

/**
 * \brief Turn a partition into an empty UBI device.
 *
 *        Writes a new volume table, then erases the old one. All volumes of
 *        the previous device are lost. Their blocks are erased later, by
 *        #UBI_MAINTENANCE_RECLAIM or when a write needs them, and stay
 *        readable from raw flash until then. Erase counts are kept. A power
 *        cut leaves the previous device or the new one.
 *
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Formatted.
 * \retval -EINVAL
 *         Invalid configuration, or the partition is unusable.
 * \retval -EACCES
 *         The key does not exist or does not allow HKDF-SHA256 derivation.
 * \retval -EBUSY
 *         The partition is in use.
 * \retval -ENOSPC
 *         The partition is too large, or has too few usable blocks.
 * \retval -ENOMEM
 *         Not enough heap.
 * \retval -EIO
 *         Flash or crypto failure.
 */
int ubi_device_format(const struct ubi_config *config);

/**
 * \brief Attach a formatted partition.
 *
 *        Reads the whole partition, verifies the UBI metadata and asks the
 *        state callback whether to trust the device. Writes nothing to the
 *        flash.
 *
 *        Format the partition only after \c -ENODEV: any other error means
 *        a UBI device may be there, and a format would destroy it. What to
 *        do on each error: docs/operations.md.
 *
 * \param[in,out] ubi                   Storage of \ref ubi_device_size bytes.
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Attached.
 * \retval -EINVAL
 *         Invalid configuration, or the partition is unusable.
 * \retval -EACCES
 *         The key does not exist or does not allow HKDF-SHA256 derivation.
 * \retval -EBUSY
 *         The handle or the partition is in use.
 * \retval -ENODEV
 *         No UBI device on the partition: it is blank or holds other data.
 * \retval -EBADMSG
 *         UBI metadata failed verification: a different key, damage or
 *         tampering.
 * \retval -ENOTSUP
 *         Written by a newer release, or with more volumes than
 *         \c CONFIG_UBI_MAX_NR_OF_VOLUMES.
 * \retval -ENOSPC
 *         The partition is too large, or its volumes do not fit in it.
 * \retval -ENOMEM
 *         Not enough heap.
 * \retval -EROFS
 *         The state callback returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash or crypto failure.
 */
int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Detach a device and destroy its derived keys.
 *
 *        Every write is already on the flash, so nothing is lost. Call it
 *        only when no other thread uses the device.
 *
 * \param[in,out] ubi                   Attached device.
 *
 * \retval 0
 *         Detached.
 * \retval -EINVAL
 *         The device is not attached.
 * \retval -EDEADLK
 *         Called from one of the device's own callbacks.
 */
int ubi_device_deinit(struct ubi_device *ubi);

/**
 * \brief Get device information.
 *
 * \param[in] ubi                       Attached device.
 * \param[out] info                     Receives the device information.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -EFAULT
 *         The handle is corrupted.
 */
int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);

/* Volume management */

/**
 * \brief Create a volume.
 *
 *        Takes \p config->leb_count logical erase blocks (LEBs) from the free
 *        LEBs of the device. Physical erase blocks are used only once the
 *        LEBs are written. Identifiers are not reused until the next format.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[in] config                    Name and size.
 * \param[out] vol_id                   Assigned identifier.
 *
 * \retval 0
 *         Created.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -EEXIST
 *         A volume with that name exists.
 * \retval -ENOSPC
 *         Not enough free LEBs, or no more volumes can be created.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure.
 */
int ubi_volume_create(struct ubi_device *ubi,
		      const struct ubi_volume_config *config, uint32_t *vol_id);

/**
 * \brief Change the size of a volume.
 *
 *        Growing adds unmapped LEBs at the end of the volume, taken from the
 *        free LEBs of the device. Shrinking removes LEBs from the end, and
 *        they have to be unmapped first with \ref ubi_leb_unmap or
 *        \ref ubi_leb_erase. For example, shrinking from 8 to 6 LEBs needs
 *        LEBs 6 and 7 unmapped.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to resize.
 * \param leb_count                     New size in LEBs, at least one.
 *
 * \retval 0
 *         Resized, or already that size.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBUSY
 *         A LEB that shrinking removes is still mapped.
 * \retval -ENOSPC
 *         Not enough free LEBs to grow.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure.
 */
int ubi_volume_resize(struct ubi_device *ubi, uint32_t vol_id,
		      uint32_t leb_count);

/**
 * \brief Remove a volume.
 *
 *        Its blocks are erased later, by #UBI_MAINTENANCE_RECLAIM or when a
 *        write needs them, and stay readable from raw flash until then.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to remove.
 *
 * \retval 0
 *         Removed.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure.
 */
int ubi_volume_remove(struct ubi_device *ubi, uint32_t vol_id);

/**
 * \brief Find a volume by name.
 *
 * \param[in] ubi                       Attached device.
 * \param[in] name                      NUL-terminated volume name.
 * \param[out] vol_id                   Identifier of the volume found.
 *
 * \retval 0
 *         Found.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No volume with that name.
 */
int ubi_volume_find(struct ubi_device *ubi, const char *name, uint32_t *vol_id);

/**
 * \brief Get volume information.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume to inspect.
 * \param[out] info                     Receives the volume information.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info);

/* Logical erase block operations */

/**
 * \brief Map a LEB to an empty physical erase block.
 *
 *        Writes a header with no data to a free physical erase block. The
 *        LEB then reads as erased, and the next attach keeps it mapped and
 *        empty. \ref ubi_leb_write_at writes into this block;
 *        \ref ubi_leb_change replaces it as usual.
 *
 *        Use it right after \ref ubi_leb_unmap, so that the next attach does
 *        not bring the old contents back, or to move the block allocation
 *        out of the first \ref ubi_leb_write_at.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Mapped.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EEXIST
 *         The LEB is already mapped.
 * \retval -ENOSPC
 *         No physical erase block is available.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure.
 */
int ubi_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Unmap a LEB from its physical erase block.
 *
 *        The LEB reads as erased from now on. Nothing is written to the
 *        flash: the old block is erased later, by #UBI_MAINTENANCE_RECLAIM
 *        or when a write needs it. Until then, the next attach brings the
 *        old contents back, unless the LEB was written again.
 *        \ref ubi_leb_map right after prevents that; \ref ubi_leb_erase
 *        erases the contents at once.
 *
 *        Unmapping an unmapped LEB does nothing.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Unmapped.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 */
int ubi_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Unmap a LEB and erase its contents now.
 *
 *        Erases the physical erase block of the LEB and every old copy of it
 *        left by \ref ubi_leb_change or \ref ubi_leb_unmap, one erase each.
 *        On return the contents are gone, and the next attach does not bring
 *        them back. On an unmapped LEB it erases only the old copies.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Erased, or there was nothing to erase.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure. The contents may come back at the next attach, and
 *         a failed erase makes the device read-only.
 */
int ubi_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Read from a LEB.
 *
 *        Unwritten bytes and unmapped LEBs read as erased flash, usually
 *        0xFF. Data is returned as stored: data written with
 *        \ref ubi_leb_change is checked at attach, not here. With
 *        \c CONFIG_UBI_VERIFY_ON_READ the block header is authenticated
 *        before each read.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param offset                        Byte offset within the LEB.
 * \param[out] buffer                   Destination buffer.
 * \param length                        Bytes to read; \p offset + \p length
 *                                      must not exceed the LEB size. Zero
 *                                      reads nothing and succeeds.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBADMSG
 *         The block header failed verification. Only with
 *         \c CONFIG_UBI_VERIFY_ON_READ.
 * \retval -EIO
 *         Flash failure.
 */
int ubi_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		 uint32_t offset, void *buffer, size_t length);

/**
 * \brief Replace the contents of a LEB atomically.
 *
 *        Writes the data to a new physical erase block, then switches the
 *        LEB to it; the old block is erased later. Bytes past \p length read
 *        as erased. After a power cut the LEB holds the old contents or the
 *        new. If there are no old contents, it holds the part of the new
 *        data that was written, and the next attach reports
 *        #UBI_EVENT_DATA_CORRUPT.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write: a multiple of the
 *                                      write block size, at most the LEB
 *                                      size. Zero does nothing.
 *
 * \retval 0
 *         The new contents are on the flash, or \p length was zero.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         No physical erase block is available.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure; the LEB keeps its old contents.
 */
int ubi_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		   const void *buffer, size_t length);

/**
 * \brief Write to a LEB at a given offset.
 *
 *        Not atomic, and no length is stored: after a power cut the
 *        application finds the end of its data itself. An unmapped LEB is
 *        mapped first.
 *
 *        Writes go in rising order: each starts at or after the end of the
 *        previous one, and a gap left below written data must stay
 *        unwritten. This holds until the LEB is changed, unmapped or erased.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param offset                        Byte offset within the LEB, a multiple
 *                                      of the write block size.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write, a multiple of the
 *                                      write block size, ending within the
 *                                      LEB. Zero does nothing.
 *
 * \retval 0
 *         The data is written, or \p length was zero.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         The LEB was unmapped and no physical erase block is available.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash failure; part of the data may be written.
 */
int ubi_leb_write_at(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint32_t offset, const void *buffer, size_t length);

/**
 * \brief Get the mapping state of a LEB.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param[out] info                     Receives the LEB state.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_leb_get_info(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     struct ubi_leb_info *info);

/* Maintenance */

/**
 * \brief Run one maintenance operation, up to a budget of steps.
 *
 *        Maintenance refills the free pool, levels wear and repairs damage.
 *        One step of each operation:
 *
 *        - #UBI_MAINTENANCE_RECLAIM erases one of the \c reclaimable_pebs
 *          and adds it to the free pool. A block released by
 *          \ref ubi_leb_unmap is erased together with every old copy of its
 *          LEB.
 *        - #UBI_MAINTENANCE_RELOCATE moves the LEB of one of the
 *          \c relocatable_pebs to a more worn free block, then erases the old
 *          block. The LEB contents do not change.
 *        - #UBI_MAINTENANCE_REPAIR rewrites the volume table when its copies
 *          differ, or erases a retired block to give it another chance.
 *        - #UBI_MAINTENANCE_DISCARD erases one of the \c corrupt_pebs and
 *          returns it to service.
 *
 *        The call ends when \p budget steps are done, nothing is left, or
 *        the device becomes read-only. Each step erases at least one block,
 *        which takes milliseconds to seconds depending on the flash.
 *
 * \param[in,out] ubi                   Attached device.
 * \param operation                     Operation to run.
 * \param budget                        Most steps to run. Zero only reports
 *                                      the work left.
 * \param[out] result                   Steps done and left. Filled on every
 *                                      return but \c -EINVAL and
 *                                      \c -EDEADLK.
 *
 * \retval 0
 *         Done, or nothing to do.
 * \retval -EINVAL
 *         Invalid argument.
 * \retval -EBADMSG
 *         A block to relocate failed verification; it stays where it is.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         returned #UBI_STATE_UNTRUSTED.
 * \retval -EFAULT
 *         Internal state is inconsistent.
 * \retval -EIO
 *         Flash failure.
 */
int ubi_maintenance(struct ubi_device *ubi, enum ubi_maintenance_op operation,
		    uint32_t budget, struct ubi_maintenance_result *result);

#endif /* UBI_H */
