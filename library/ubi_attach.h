/**
 * \file    ubi_attach.h
 * \author  Kamil Kielbasa
 * \brief   Rebuilding a device in RAM from what its partition holds.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_ATTACH_H
#define UBI_ATTACH_H

/* Include files ----------------------------------------------------------- */

/* UBI headers: */
#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Scan the partition, adopt the volume table in force and rebuild the
 *        volumes and every block's state. Reads only.
 *
 * \param[in,out] ubi                   Device with its partition open and its
 *                                      per-block bookkeeping allocated.
 *
 * \retval 0
 *         Rebuilt.
 * \retval -ENODEV
 *         No UBI metadata on the partition.
 * \retval -EBADMSG
 *         UBI metadata that will not verify.
 * \retval -ENOTSUP
 *         Written by a release this build cannot read.
 * \retval -EINVAL
 *         The geometry differs from the one formatted, or too many blocks
 *         are corrupt.
 * \retval -ENOSPC
 *         The volume table declares more logical blocks than there are.
 * \retval -EIO
 *         A block could not be read, or the crypto backend failed.
 */
int ubi_impl_attach(struct ubi_device *ubi);

#endif /* UBI_ATTACH_H */
