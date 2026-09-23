/**
 * \file    ubi_state.c
 * \author  Kamil Kielbasa
 * \brief   What the device looks like, and whether it is still trusted.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>

/* UBI headers: */
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_state.h"
#include "ubi_volume.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module interface function definitions ----------------------------------- */

void ubi_impl_event_emit(const struct ubi_device *ubi, enum ubi_event_type type,
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

int ubi_impl_state_describe(const struct ubi_device *ubi,
			    struct ubi_device_info *info)
{
	struct ubi_device_info measured = {
		.peb_count = ubi->geometry.peb_count,
		.peb_size = ubi->geometry.peb_size,
		.leb_size = ubi->geometry.leb_size,
		.write_block_size = ubi->geometry.write_block_size,
		.volume_count = ubi->volumes.count,
		.image_seq = ubi->image_seq,
		.revision = ubi->volumes.revision,
		.global_sqnum = ubi->global_sqnum,
	};
	uint32_t lowest_erase_count = UINT32_MAX;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);
		const uint32_t erase_count = ubi->blocks.erase_count[pnum];

		switch (state) {
		case UBI_PEB_FREE:
			measured.free_pebs += 1;
			break;
		case UBI_PEB_UNKNOWN:
		case UBI_PEB_RECLAIM:
			measured.reclaimable_pebs += 1;
			break;
		case UBI_PEB_BAD:
		case UBI_PEB_WORN_OUT:
			measured.bad_pebs += 1;
			break;
		case UBI_PEB_CORRUPT:
			measured.corrupt_pebs += 1;
			break;
		case UBI_PEB_MAPPED:
			break;
		default:
			return -EFAULT;
		}

		/* Every other state means the erase counter header verified
		 * and named this image, so its count can be added up. */
		if (UBI_PEB_UNKNOWN == state || UBI_PEB_BAD == state ||
		    UBI_PEB_WORN_OUT == state)
			continue;

		measured.healthy_pebs += 1;
		measured.total_erase_count += erase_count;

		if (erase_count < lowest_erase_count)
			lowest_erase_count = erase_count;

		if (erase_count > measured.max_erase_count)
			measured.max_erase_count = erase_count;
	}

	if (0 != measured.healthy_pebs)
		measured.min_erase_count = lowest_erase_count;

	/* A second pass, because a candidate is judged against the highest
	 * erase count and that is only known once the first one is over. */
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const uint32_t erase_count = ubi->blocks.erase_count[pnum];

		if (UBI_PEB_MAPPED != ubi_impl_peb_state_get(ubi, pnum))
			continue;

		if (measured.max_erase_count - erase_count >
		    CONFIG_UBI_WEAR_LEVELING_THRESHOLD)
			measured.relocatable_pebs += 1;
	}

	measured.free_lebs = ubi_impl_volumes_leb_free(ubi);

	*info = measured;

	return 0;
}

int ubi_impl_state_check(struct ubi_device *ubi)
{
	if (ubi->untrusted)
		return -EROFS;

	struct ubi_device_info info = { 0 };
	const int ret = ubi_impl_state_describe(ubi, &info);

	if (0 != ret) {
		LOG_ERR("the block state table is not one this build wrote");
		return ret;
	}

	ubi->writes_since_check = 0;

	if (UBI_STATE_TRUSTED ==
	    ubi->callbacks.state(&info, ubi->callbacks.user_context))
		return 0;

	LOG_ERR("the application has withdrawn its trust in this device");
	ubi->untrusted = true;

	return -EROFS;
}

int ubi_impl_state_guard(struct ubi_device *ubi)
{
	if (ubi->untrusted)
		return -EROFS;

	if (CONFIG_UBI_STATE_CHECK_INTERVAL > ubi->writes_since_check)
		return 0;

	return ubi_impl_state_check(ubi);
}
