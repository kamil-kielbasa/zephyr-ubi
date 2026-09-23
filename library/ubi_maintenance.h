/**
 * \file    ubi_maintenance.h
 * \author  Kamil Kielbasa
 * \brief   Work the device puts off until the application has time for it.
 *
 *          Erasing costs milliseconds and moving a block costs a read and a
 *          write, so neither happens on the path that asked for it. They
 *          happen here, when the application says it can afford the latency.
 *
 *          Internal to the library: the boundary in ubi_api.c has already
 *          checked what arrives here.
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
 * \retval -EINVAL
 *         No such operation.
 * \retval -ENOSPC
 *         Relocation found no block to move data into.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_maintenance(struct ubi_device *ubi,
			 enum ubi_maintenance_op operation, uint32_t budget,
			 struct ubi_maintenance_result *result);

#endif /* UBI_MAINTENANCE_H */
