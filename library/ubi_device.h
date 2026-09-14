/**
 * \file    ubi_device.h
 * \author  Kamil Kielbasa
 * \brief   Turning a partition into a device, and back.
 *
 *          Formatting writes the first volume table; attaching reads every
 *          block and rebuilds in RAM what the flash says. Both are here
 *          because both answer the same question: what is on this partition?
 *
 *          Internal to the library: the boundary in ubi_api.c has already
 *          checked what arrives here.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_DEVICE_H
#define UBI_DEVICE_H

/* Include files ----------------------------------------------------------- */

/* UBI headers: */
#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Turn a partition into an empty UBI device.
 *
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Formatted.
 * \retval -EACCES
 *         The key handle may not derive.
 * \retval -EINVAL
 *         The partition geometry is one this build cannot manage.
 * \retval -ENOSPC
 *         Too few blocks, too many, or none that could hold the record.
 * \retval -ENOMEM
 *         No heap for a device handle.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_device_format_partition(const struct ubi_config *config);

/**
 * \brief Rebuild a device in RAM from what the partition holds.
 *
 *        Never writes to the flash, so a wrong key leaves the device exactly
 *        as it was.
 *
 * \param[in,out] ubi                   Storage for the handle.
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Attached.
 * \retval -EACCES
 *         The key handle may not derive.
 * \retval -EINVAL
 *         The geometry disagrees with the volume table.
 * \retval -ENODEV
 *         No usable volume table; this is not a UBI device.
 * \retval -EBADMSG
 *         Headers are present but will not verify.
 * \retval -ENOSPC
 *         More blocks than a block number can address.
 * \retval -ENOMEM
 *         No heap for the per-block bookkeeping.
 * \retval -EROFS
 *         The application withdrew its trust.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_device_attach(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Give back everything attaching took.
 *
 * \param[in,out] ubi                   Attached device.
 */
void ubi_device_detach(struct ubi_device *ubi);

#endif /* UBI_DEVICE_H */
