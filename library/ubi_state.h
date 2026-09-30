/**
 * \file    ubi_state.h
 * \author  Kamil Kielbasa
 * \brief   What the device looks like, whether it is still trusted, and what
 *          it has to report.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_STATE_H
#define UBI_STATE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_private.h"

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Hand one event to the application.
 *
 * \param[in] ubi                       Device the event concerns.
 * \param type                          What happened.
 * \param pnum                          Physical erase block concerned.
 * \param vol_id                        Volume, or #UBI_VOL_ID_INVALID.
 * \param lnum                          Logical erase block, meaningful only
 *                                      together with \p vol_id.
 */
void ubi_impl_event_emit(struct ubi_device *ubi, enum ubi_event_type type,
			 uint32_t pnum, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Take stock of the device as it stands.
 *
 * \param[in] ubi                       Device to describe.
 * \param[out] info                     Receives the picture.
 *
 * \retval 0
 *         Described.
 * \retval -EFAULT
 *         A block carries a state UBI never wrote.
 */
int ubi_impl_state_describe(const struct ubi_device *ubi,
			    struct ubi_device_info *info);

/**
 * \brief Ask the application whether it still trusts the device. A refusal
 *        is latched.
 *
 * \param[in,out] ubi                   Device to ask about.
 *
 * \retval 0
 *         Trusted.
 * \retval -EROFS
 *         Not trusted, now or earlier.
 * \retval -EFAULT
 *         The device could not be described.
 */
int ubi_impl_state_check(struct ubi_device *ubi);

/**
 * \brief Clear an operation that is about to write.
 *
 *        Refuses once trust has been withdrawn or an erase has failed, and
 *        asks again once \c CONFIG_UBI_STATE_CHECK_INTERVAL flash writes
 *        have gone by, before anything is written.
 *
 * \param[in,out] ubi                   Device about to be written.
 *
 * \retval 0
 *         Go ahead.
 * \retval -EROFS
 *         Not trusted, or read-only since an erase failed.
 * \retval -EFAULT
 *         The device could not be described.
 */
int ubi_impl_state_guard(struct ubi_device *ubi);

/**
 * \brief Check that the bookkeeping agrees with itself: every mapping names
 *        a block in use, every block in use is named exactly once, and the
 *        volumes share the mapping pool without gaps.
 *
 * \param[in] ubi                       Attached device, with
 *                                      \c CONFIG_UBI_SELF_CHECKS.
 *
 * \retval 0
 *         It does.
 * \retval -EFAULT
 *         It does not.
 */
int ubi_impl_state_self_check(const struct ubi_device *ubi);

#endif /* UBI_STATE_H */
