/**
 * \file    ubi_maintenance.h
 * \author  Kamil Kielbasa
 * \brief   Work the device puts off until the application has time for it.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_MAINTENANCE_H
#define UBI_MAINTENANCE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Carry out up to \p budget units of deferred work.
 *
 * \param[in,out] ubi                   Attached device.
 * \param operation                     Work to perform.
 * \param budget                        Units to perform; zero only counts
 *                                      what is waiting.
 * \param[out] result                   Work done and still waiting.
 *
 * \retval 0
 *         Done, including when there was nothing to do.
 * \retval -EBADMSG
 *         Relocation found a block it could not verify and left it where it
 *         is.
 * \retval -EFAULT
 *         Relocation found a block in use that no mapping names.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_maintenance(struct ubi_device *ubi,
			 enum ubi_maintenance_op operation, uint32_t budget,
			 struct ubi_maintenance_result *result);

/**
 * \brief Report that no work was done, and how much is waiting.
 *
 * \param[in] ubi                       Attached device.
 * \param operation                     Work asked for.
 * \param[out] result                   Work done and still waiting.
 */
void ubi_impl_maintenance_report(const struct ubi_device *ubi,
				 enum ubi_maintenance_op operation,
				 struct ubi_maintenance_result *result);

#endif /* UBI_MAINTENANCE_H */
