/**
 * \file    ubi_event.c
 * \author  Kamil Kielbasa
 * \brief   Telling the application what UBI found.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_event.h"
#include "ubi_private.h"

/* Module interface function definitions ----------------------------------- */

void ubi_event_emit(const struct ubi_device *ubi, enum ubi_event_type type,
		    uint32_t pnum, uint32_t vol_id, uint32_t lnum)
{
	const struct ubi_event event = {
		.type = type,
		.pnum = pnum,
		.vol_id = vol_id,
		.lnum = lnum,
	};

	ubi->callbacks.event(&event, ubi->callbacks.user_context);
}
