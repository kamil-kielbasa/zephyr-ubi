/**
 * \file    ubi_volume.h
 * \author  Kamil Kielbasa
 * \brief   Volumes and the logical-to-physical mapping.
 *
 *          A volume is a name, a size in logical erase blocks, and a slice of
 *          the erase block association table. What backs each of those blocks
 *          is decided here; what is written into them is decided a layer up.
 *
 *          The volume table has a volume of its own, reachable through
 *          \ref ubi_impl_volume_by_id but never through the public API, because
 *          the record it holds is what declares the others.
 *
 *          Internal to the library: the boundary in ubi_api.c has already
 *          checked what arrives here.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_VOLUME_H
#define UBI_VOLUME_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* UBI headers: */
#include "ubi_private.h"
#include "ubi_volume_table.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Look up a volume by identifier, the internal one included.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Identifier to look for.
 *
 * \return The volume, or \c NULL when no volume carries that identifier.
 */
struct ubi_volume *ubi_impl_volume_by_id(struct ubi_device *ubi,
					 uint32_t vol_id);

/**
 * \brief Report how many logical blocks the volumes may still claim.
 *
 * \param[in] ubi                       Attached device.
 *
 * \return Blocks left in the shared pool.
 */
uint32_t ubi_impl_volumes_leb_free(const struct ubi_device *ubi);

/**
 * \brief Write the volumes as they stand to every copy of the volume table.
 *
 *        What an update does anyway, without anything to update: the way to
 *        bring a pair that does not agree back into agreement.
 *
 * \param[in,out] ubi                   Attached device.
 *
 * \retval 0
 *         Both copies now carry the same record.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_volumes_rewrite(struct ubi_device *ubi);

/**
 * \brief Give every volume a record declares its slice of the mapping table.
 *
 *        Leaves every logical block unmapped; the second scan pass hangs the
 *        physical blocks off them.
 *
 * \param[in,out] ubi                   Device being attached.
 * \param[in] record                    Record to build from.
 *
 * \retval 0
 *         Built.
 * \retval -ENOSPC
 *         The record declares more logical blocks than the partition can
 *         share out.
 */
int ubi_impl_volumes_build(struct ubi_device *ubi,
			   const struct ubi_volume_table_record *record);

/**
 * \brief Report which physical block backs a logical one, or
 *        #UBI_LEB_UNMAPPED when none does.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 * \param[out] pnum                     Block behind it.
 *
 * \retval 0
 *         Answered.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EINVAL
 *         The volume does not reach that far.
 */
int ubi_impl_volume_leb_get(struct ubi_device *ubi, uint32_t vol_id,
			    uint32_t lnum, uint16_t *pnum);

/**
 * \brief Point a logical block at a physical one, with the same contract.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block.
 * \param pnum                          Block to put behind it, or
 *                                      #UBI_LEB_UNMAPPED to break the link.
 *
 * \retval 0
 *         Mapped.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EINVAL
 *         The volume does not reach that far.
 */
int ubi_impl_volume_leb_set(struct ubi_device *ubi, uint32_t vol_id,
			    uint32_t lnum, uint16_t pnum);

/**
 * \brief Declare a volume and put it in the volume table.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[in] config                    Name and size.
 * \param[out] vol_id                   Identifier assigned.
 *
 * \retval 0
 *         Created.
 * \retval -EINVAL
 *         The name is empty, too long, or one no volume may carry.
 * \retval -EEXIST
 *         A volume already carries that name.
 * \retval -ENOSPC
 *         Not enough logical blocks, or no identifier or table entry left.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_volume_create(struct ubi_device *ubi,
			   const struct ubi_volume_config *config,
			   uint32_t *vol_id);

/**
 * \brief Strike a volume from the volume table and release its blocks.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to remove.
 *
 * \retval 0
 *         Removed.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_volume_remove(struct ubi_device *ubi, uint32_t vol_id);

/**
 * \brief Give a volume a new size, taking from or handing back to the pool.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to resize.
 * \param leb_count                     New size, at least one block.
 *
 * \retval 0
 *         Resized, or already that size.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBUSY
 *         A logical block above \p leb_count is still mapped.
 * \retval -ENOSPC
 *         The pool has fewer logical blocks left than the growth asks for.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_volume_resize(struct ubi_device *ubi, uint32_t vol_id,
			   uint32_t leb_count);

/**
 * \brief Look up a volume identifier by name, the internal one excluded.
 *
 * \param[in] ubi                       Attached device.
 * \param[in] name                      Name to look for.
 * \param[out] vol_id                   Identifier found.
 *
 * \retval 0
 *         Found.
 * \retval -ENOENT
 *         No volume carries that name.
 */
int ubi_impl_volume_find(const struct ubi_device *ubi, const char *name,
			 uint32_t *vol_id);

/**
 * \brief Take stock of one volume.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume to describe.
 * \param[out] info                     Receives the properties.
 *
 * \retval 0
 *         Described.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_impl_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			     struct ubi_volume_info *info);

#endif /* UBI_VOLUME_H */
