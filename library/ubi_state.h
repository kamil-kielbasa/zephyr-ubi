/**
 * \file    ubi_state.h
 * \author  Kamil Kielbasa
 * \brief   What the device looks like, and whether it is still trusted.
 *
 *          The two belong together: the application decides whether to trust
 *          the flash from exactly the picture this module paints, so there is
 *          one place that takes stock and one place that asks.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_STATE_H
#define UBI_STATE_H

/* Include files ----------------------------------------------------------- */

/* UBI headers: */
#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Take stock of the device as it stands.
 *
 * \param[in] ubi                       Device to describe.
 * \param[out] info                     Receives the picture.
 *
 * \retval 0
 *         Described.
 * \retval -EFAULT
 *         A block carries a state UBI never wrote, so the handle has been
 *         corrupted and the counts would be a lie.
 */
int ubi_state_describe(const struct ubi_device *ubi,
		       struct ubi_device_info *info);

/**
 * \brief Ask the application whether it still trusts the device.
 *
 *        A refusal is latched, so that retrying is not a way around it.
 *
 * \param[in,out] ubi                   Device to ask about.
 *
 * \retval 0
 *         Trusted.
 * \retval -EROFS
 *         Not trusted, now or earlier.
 * \retval -EFAULT
 *         The picture could not be painted, so there was nothing to ask
 *         about.
 */
int ubi_state_check(struct ubi_device *ubi);

/**
 * \brief Clear an operation that is about to write.
 *
 *        Refuses outright once trust has been withdrawn, and asks again once
 *        \c CONFIG_UBI_STATE_CHECK_INTERVAL metadata writes have gone by.
 *        Asking before the write rather than after is what lets a refusal be
 *        honoured with nothing written.
 *
 * \param[in,out] ubi                   Device about to be written.
 *
 * \retval 0
 *         Go ahead.
 * \retval -EROFS
 *         Not trusted.
 * \retval -EFAULT
 *         The handle has been corrupted.
 */
int ubi_state_guard(struct ubi_device *ubi);

#endif /* UBI_STATE_H */
