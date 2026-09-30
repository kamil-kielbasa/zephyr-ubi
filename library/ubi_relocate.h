/**
 * \file    ubi_relocate.h
 * \author  Kamil Kielbasa
 * \brief   Moving data off little-worn blocks so that they take their share
 *          of the erases.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_RELOCATE_H
#define UBI_RELOCATE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* UBI headers: */
#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Count the blocks worn so much less than the free block they would
 *        move onto that moving them is worth an erase.
 *
 * \param[in] ubi                       Attached device.
 *
 * \return How many there are.
 */
uint32_t ubi_impl_relocate_pending(const struct ubi_device *ubi);

/**
 * \brief Move the least worn of them onto the most worn free block, switch
 *        the mapping and erase the source.
 *
 * \param[in,out] ubi                   Attached device.
 *
 * \retval 0
 *         Moved.
 * \retval -ENOENT
 *         Nothing is worth moving, or no block is free to move onto.
 * \retval -EBADMSG
 *         The source does not verify, or reads back differently each time;
 *         it stays where it is and is not chosen again.
 * \retval -EFAULT
 *         The source is in use but no mapping names it.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_relocate_step(struct ubi_device *ubi);

#endif /* UBI_RELOCATE_H */
