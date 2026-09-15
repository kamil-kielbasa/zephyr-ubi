/**
 * \file    ubi_event.h
 * \author  Kamil Kielbasa
 * \brief   Telling the application what UBI found.
 *
 *          Every module that can discover damage reports it the same way, so
 *          the application hears one voice regardless of which layer noticed.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_EVENT_H
#define UBI_EVENT_H

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
void ubi_event_emit(const struct ubi_device *ubi, enum ubi_event_type type,
		    uint32_t pnum, uint32_t vol_id, uint32_t lnum);

#endif /* UBI_EVENT_H */
