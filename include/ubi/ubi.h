/**
 * \file    ubi.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) public API.
 *
 *          Maps logical erase blocks (LEB) onto physical erase blocks (PEB),
 *          spreads wear across the partition, survives power loss and
 *          authenticates its own metadata with AES-CMAC. Application data is
 *          stored as it is given. Key material enters as a PSA key handle.
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

#ifdef __cplusplus
extern "C" {
#endif

/* Module interface function declarations ---------------------------------- */

/** \defgroup ubi-api-device UBI device lifecycle
 * @{
 */

/**
 * \brief Size in bytes of a UBI device handle.
 *
 *        Allocate at least this many bytes for the \ref ubi_device passed to
 *        \ref ubi_device_init. Everything that grows with the partition is
 *        taken from the heap when the device attaches.
 *
 * \return Size in bytes of \ref ubi_device.
 */
size_t ubi_device_size(void);

/**
 * \brief Turn a partition into an empty UBI device.
 *
 *        Writes a fresh image sequence number and both volume table copies,
 *        and erases every volume table an earlier format left. Other blocks
 *        belong to no device any more and are erased when first needed; until
 *        then they stay readable from raw flash, so run
 *        #UBI_MAINTENANCE_RECLAIM to the end where that matters. Erase counts
 *        are carried over. A format cut short leaves the earlier device or
 *        the new one.
 *
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Formatted.
 * \retval -EINVAL
 *         \p config is incomplete, its key context does not fit, or the
 *         partition is not made of whole erase blocks of one size.
 * \retval -EACCES
 *         \p ikm_key_id is missing, or may not derive with HKDF-SHA256.
 * \retval -EBUSY
 *         The partition is attached, or another format of it is running.
 * \retval -ENOSPC
 *         The partition holds fewer blocks than UBI needs, or more than a
 *         16-bit block number addresses.
 * \retval -ENOMEM
 *         Not enough heap.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_device_format(const struct ubi_config *config);

/**
 * \brief Attach a formatted partition.
 *
 *        Reads every block, verifies every header and the data of every
 *        sealed block, rebuilds the logical-to-physical map in RAM and asks
 *        the state callback whether to trust the result. Writes nothing, so a
 *        failed attach leaves the flash as it was.
 *
 *        Only \c -ENODEV means there is nothing on the partition to lose.
 *
 * \param[in,out] ubi                   Storage of \ref ubi_device_size bytes.
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Attached.
 * \retval -EINVAL
 *         \p config is incomplete, its key context does not fit, the
 *         partition is not made of whole erase blocks of one size, its
 *         geometry differs from the one formatted, or too many blocks are
 *         corrupt.
 * \retval -EACCES
 *         \p ikm_key_id is missing, or may not derive with HKDF-SHA256.
 * \retval -EBUSY
 *         \p ubi is attached already, or the partition is attached or being
 *         formatted.
 * \retval -ENODEV
 *         No UBI metadata at all: the partition is blank or holds someone
 *         else's bytes. \ref ubi_device_format it if that is expected.
 * \retval -EBADMSG
 *         UBI metadata that will not verify: the wrong key, both volume
 *         table copies damaged, or data with no volume table left.
 * \retval -ENOTSUP
 *         Written by a release this build cannot read, or with more volumes
 *         than \c CONFIG_UBI_MAX_NR_OF_VOLUMES allows.
 * \retval -ENOSPC
 *         More blocks than a 16-bit block number addresses, or a volume
 *         table that declares more logical blocks than the partition has.
 * \retval -ENOMEM
 *         Not enough heap for the bookkeeping of this partition.
 * \retval -EROFS
 *         The state callback returned #UBI_STATE_UNTRUSTED.
 * \retval -EIO
 *         Flash driver or crypto backend failure. A block that cannot be
 *         read says nothing about what it holds; try again.
 */
int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Detach a device and destroy its derived keys.
 *
 *        Loses nothing: everything UBI needs is on the flash. The next attach
 *        finds again what was waiting for reclaim, maps back a block unmapped
 *        since, gives retired blocks another chance and lifts a read-only
 *        state. No other thread may use \p ubi during or after the call.
 *
 * \param[in,out] ubi                   Attached device.
 *
 * \retval 0
 *         Detached.
 * \retval -EINVAL
 *         \p ubi is not attached.
 * \retval -EDEADLK
 *         Called from one of the device's own callbacks.
 */
int ubi_device_deinit(struct ubi_device *ubi);

/**
 * \brief Read geometry, block accounting and rollback counters, from RAM.
 *
 * \param[in] ubi                       Attached device.
 * \param[out] info                     Receives the device state.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p info is \c NULL.
 * \retval -EFAULT
 *         A block carries a state UBI never wrote: the handle is corrupted.
 */
int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);

/**@}*/

/** \defgroup ubi-api-volume UBI volume management
 * @{
 */

/**
 * \brief Create a volume and write it to the volume table.
 *
 *        Reserves \p leb_count logical blocks but takes no physical ones
 *        until they are written. The identifier is never reused.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[in] config                    Name and size.
 * \param[out] vol_id                   Assigned identifier.
 *
 * \retval 0
 *         Created.
 * \retval -EINVAL
 *         \p ubi is not attached, the name is empty or too long, or the size
 *         is zero.
 * \retval -EEXIST
 *         A volume with that name exists.
 * \retval -ENOSPC
 *         Not enough logical blocks left, or the volume limit is reached.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_volume_create(struct ubi_device *ubi,
		      const struct ubi_volume_config *config, uint32_t *vol_id);

/**
 * \brief Give a volume a new size in logical blocks.
 *
 *        Growing takes from the shared pool and adds unmapped blocks;
 *        shrinking hands blocks back and is refused while one past the new
 *        size is still mapped.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to resize.
 * \param leb_count                     New size, at least one block.
 *
 * \retval 0
 *         Resized, or already that size.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p leb_count is zero.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBUSY
 *         A logical block past \p leb_count is still mapped.
 * \retval -ENOSPC
 *         The pool has fewer logical blocks left than the growth asks for.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_volume_resize(struct ubi_device *ubi, uint32_t vol_id,
		      uint32_t leb_count);

/**
 * \brief Remove a volume and release its blocks.
 *
 *        The blocks are queued for #UBI_MAINTENANCE_RECLAIM, not erased, and
 *        stay readable from raw flash until they are.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to remove.
 *
 * \retval 0
 *         Removed.
 * \retval -EINVAL
 *         \p ubi is not attached.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_volume_remove(struct ubi_device *ubi, uint32_t vol_id);

/**
 * \brief Look up a volume identifier by name.
 *
 *        Identifiers change with every \ref ubi_device_format, so resolve
 *        them by name after each attach.
 *
 * \param[in] ubi                       Attached device.
 * \param[in] name                      NUL-terminated volume name.
 * \param[out] vol_id                   Identifier of the volume found.
 *
 * \retval 0
 *         Found.
 * \retval -EINVAL
 *         \p ubi is not attached, or an argument is \c NULL.
 * \retval -ENOENT
 *         No volume with that name.
 */
int ubi_volume_find(struct ubi_device *ubi, const char *name, uint32_t *vol_id);

/**
 * \brief Read a volume's properties.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume to inspect.
 * \param[out] info                     Receives the volume properties.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p info is \c NULL.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info);

/**@}*/

/** \defgroup ubi-api-leb UBI logical erase block operations
 * @{
 */

/**
 * \brief Give a logical erase block a physical one, without writing data.
 *
 *        The block reads as erased afterwards, and stays mapped across an
 *        unclean reboot. \ref ubi_leb_write_at and \ref ubi_leb_change map on
 *        their own.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Mapped.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p lnum is out of range.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EEXIST
 *         The block is mapped already. Linux UBI returns \c -EBADMSG, which
 *         here only ever means a failed authentication.
 * \retval -ENOSPC
 *         No physical block available.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Detach a logical erase block from its physical one.
 *
 *        Drops the mapping in RAM and queues the block for
 *        #UBI_MAINTENANCE_RECLAIM. Afterwards the LEB reads as erased and
 *        the next write takes another block. Unmapping an unmapped LEB does
 *        nothing.
 *
 *        **Nothing is written to the flash.** Until the queued erase runs,
 *        the block still names this LEB and the next attach maps it back,
 *        with the contents it had when unmapped and never older ones. Use
 *        \ref ubi_leb_erase where that matters.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Unmapped.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p lnum is out of range.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 */
int ubi_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Detach a logical erase block and erase it now.
 *
 *        What \ref ubi_leb_unmap promises eventually, this promises on
 *        return: the block is erased, and so is every older copy of it still
 *        waiting for reclaim, one an earlier unmap left included, so the
 *        contents are gone for good. Costs one erase per copy. An erase that
 *        fails leaves the device read-only, and no copy that could not be
 *        erased is ever reported gone.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Erased, or there was nothing to erase.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p lnum is out of range.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure. The block that could not be erased is
 *         retired, the device is read-only, and what it held may come back
 *         after a reboot.
 */
int ubi_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Read from a logical erase block.
 *
 *        One flash read, since the mapping is in RAM. With
 *        \c CONFIG_UBI_VERIFY_ON_READ the header is read and verified first,
 *        and has to name this logical block in this image. The data itself
 *        is judged at attach. Bytes never written, and an unmapped LEB, read
 *        as erased.
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
 *         \p ubi is not attached, \p lnum is out of range, or the range
 *         spills past the end of the LEB.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBADMSG
 *         Only with \c CONFIG_UBI_VERIFY_ON_READ: the header failed its CMAC
 *         or names another logical block or another image.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		 uint32_t offset, void *buffer, size_t length);

/**
 * \brief Replace the contents of a logical erase block atomically.
 *
 *        Writes a fresh physical block and moves the mapping only once the
 *        data is down, so a power loss leaves the old contents or the new
 *        ones. A LEB that had no contents keeps what reached the flash, and
 *        the next attach reports a partial copy with #UBI_EVENT_DATA_CORRUPT.
 *        The old block is queued for reclaim.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write: whole write blocks,
 *                                      at most the LEB size. Zero does
 *                                      nothing.
 *
 * \retval 0
 *         The new contents are durable, or \p length was zero.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, or \p length is
 *         not whole write blocks within the LEB.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         No physical block available.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure; the LEB still holds its previous contents.
 */
int ubi_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		   const void *buffer, size_t length);

/**
 * \brief Write to a logical erase block at a caller-chosen offset.
 *
 *        Places the bytes where asked and promises nothing more:
 *
 *        - \p offset and \p length are whole write blocks.
 *        - An unmapped LEB is mapped on the way.
 *        - Writes to one LEB go in rising order, each at or past the end of
 *          the previous one, until the next \ref ubi_leb_change,
 *          \ref ubi_leb_unmap or \ref ubi_leb_erase. Relocation seals a block
 *          up to its last written byte, and flash cannot take a second write
 *          over written bytes.
 *        - No length is stored: after a reboot the application finds its
 *          own frontier and detects a partial append itself.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param offset                        Byte offset within the LEB.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write. Zero does nothing.
 *
 * \retval 0
 *         The bytes were written, or \p length was zero.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, the range spills
 *         past the end of the LEB, or it is not whole write blocks.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         The LEB was unmapped and no physical block was available.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_write_at(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint32_t offset, const void *buffer, size_t length);

/**
 * \brief Read the mapping state of a logical erase block, from RAM.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param[out] info                     Receives the block state.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, or \p info is
 *         \c NULL.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_leb_get_info(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     struct ubi_leb_info *info);

/**@}*/

/** \defgroup ubi-api-maintenance UBI maintenance
 * @{
 */

/**
 * \brief Perform deferred housekeeping, up to a caller-set budget.
 *
 *        There is no background thread: erasing and relocating happen here,
 *        when the application can afford the latency. An erase takes tens of
 *        milliseconds to seconds, so a budget of one is a reasonable slice.
 *        A reclaim step may erase every copy of the logical block an unmap
 *        let go of. A relocation step copies a block, reads the copy back and
 *        erases the old one; the LEB reads the same before and after.
 *
 *        A step that has taken effect stands even when the erase it ends
 *        with fails; the device is then read-only and the call stops.
 *
 * \param[in,out] ubi                   Attached device.
 * \param operation                     Work to perform.
 * \param budget                        Most operations to perform. Zero only
 *                                      reports what is pending.
 * \param[out] result                   Work done and remaining, filled on
 *                                      every return but \c -EINVAL and
 *                                      \c -EDEADLK.
 *
 * \retval 0
 *         Success, including when there was nothing to do.
 * \retval -EINVAL
 *         \p ubi is not attached, \p operation is unknown, or \p result is
 *         \c NULL.
 * \retval -EBADMSG
 *         Relocation found a block whose header does not verify, or whose
 *         data reads differently each time. It stays where it is, serving
 *         its LEB, and is not moved again until the next attach.
 * \retval -EROFS
 *         The device is read-only: an erase failed, or the state callback
 *         withdrew its trust.
 * \retval -EFAULT
 *         Relocation found a block in use that backs no LEB: the bookkeeping
 *         contradicts itself.
 * \retval -EIO
 *         Flash driver failure. If it was an erase, the device is read-only
 *         from then on.
 */
int ubi_maintenance(struct ubi_device *ubi, enum ubi_maintenance_op operation,
		    uint32_t budget, struct ubi_maintenance_result *result);

/**@}*/

#ifdef __cplusplus
}
#endif

#endif /* UBI_H */
