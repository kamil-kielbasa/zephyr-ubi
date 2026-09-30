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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_relocate.h"
#include "ubi_state.h"
#include "ubi_volume.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Check one volume's mappings, marking in \p named the blocks they
 *        name.
 */
static int state_volume_check(const struct ubi_device *ubi,
			      const struct ubi_volume *volume, uint8_t *named);

/* Static function definitions --------------------------------------------- */

static int state_volume_check(const struct ubi_device *ubi,
			      const struct ubi_volume *volume, uint8_t *named)
{
	for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum) {
		const uint16_t pnum = volume->eba[lnum];

		if (UBI_LEB_UNMAPPED == pnum)
			continue;

		if (pnum >= ubi->geometry.peb_count) {
			LOG_ERR("volume %u block %u names PEB %u, past the "
				"partition",
				volume->vol_id, lnum, pnum);
			return -EFAULT;
		}

		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_MAPPED != state && UBI_PEB_ERRONEOUS != state) {
			LOG_ERR("volume %u block %u names PEB %u, which is in "
				"state %d",
				volume->vol_id, lnum, pnum, state);
			return -EFAULT;
		}

		if (0 !=
		    (named[pnum / BITS_PER_BYTE] & BIT(pnum % BITS_PER_BYTE))) {
			LOG_ERR("volume %u block %u names PEB %u, which backs "
				"another logical block too",
				volume->vol_id, lnum, pnum);
			return -EFAULT;
		}

		named[pnum / BITS_PER_BYTE] |=
			(uint8_t)BIT(pnum % BITS_PER_BYTE);
	}

	return 0;
}

/* Module interface function definitions ----------------------------------- */

void ubi_impl_event_emit(struct ubi_device *ubi, enum ubi_event_type type,
			 uint32_t pnum, uint32_t vol_id, uint32_t lnum)
{
	const struct ubi_event event = {
		.type = type,
		.pnum = pnum,
		.vol_id = vol_id,
		.lnum = lnum,
	};

	ubi->in_callback = true;
	ubi->callbacks.event(&event, ubi->callbacks.user_context);
	ubi->in_callback = false;
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
		.max_sqnum = ubi->max_sqnum,
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
		case UBI_PEB_UNMAPPED:
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
		case UBI_PEB_ERRONEOUS:
			break;
		default:
			return -EFAULT;
		}

		/* Every other state means the erase counter header verified
		 * and named this image, so its count is known. */
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

	measured.relocatable_pebs = ubi_impl_relocate_pending(ubi);
	measured.free_lebs = ubi_impl_volumes_leb_free(ubi);

	*info = measured;

	return 0;
}

int ubi_impl_state_check(struct ubi_device *ubi)
{
	struct ubi_device_info info = { 0 };
	int ret = 0;

	if (ubi->untrusted)
		return -EROFS;

	ret = ubi_impl_state_describe(ubi, &info);

	if (0 != ret) {
		LOG_ERR("the block state table is not one this build wrote");
		return ret;
	}

	ubi->writes_since_check = 0;

	ubi->in_callback = true;

	const enum ubi_state_verdict verdict =
		ubi->callbacks.state(&info, ubi->callbacks.user_context);

	ubi->in_callback = false;

	if (UBI_STATE_TRUSTED == verdict)
		return 0;

	LOG_ERR("the application has withdrawn its trust in this device");
	ubi->untrusted = true;

	return -EROFS;
}

int ubi_impl_state_guard(struct ubi_device *ubi)
{
	if (ubi->untrusted || ubi->read_only)
		return -EROFS;

	if (CONFIG_UBI_STATE_CHECK_INTERVAL > ubi->writes_since_check)
		return 0;

	return ubi_impl_state_check(ubi);
}

int ubi_impl_state_self_check(const struct ubi_device *ubi)
{
	uint8_t *named = ubi->blocks.named;
	uint32_t used = 0;
	int ret = 0;

	memset(named, 0, UBI_NAMED_SIZE(ubi->geometry.peb_count));

	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		const struct ubi_volume *volume = &ubi->volumes.entries[i];

		if (volume->eba != &ubi->volumes.eba_pool[used]) {
			LOG_ERR("volume %u does not own the mappings behind "
				"the volume before it",
				volume->vol_id);
			return -EFAULT;
		}

		used += volume->leb_count;

		ret = state_volume_check(ubi, volume, named);

		if (0 != ret)
			return ret;
	}

	if (used != ubi->volumes.eba_used) {
		LOG_ERR("the volumes own %u mappings but %u are handed out",
			used, ubi->volumes.eba_used);
		return -EFAULT;
	}

	ret = state_volume_check(ubi, &ubi->volume_table.volume, named);

	if (0 != ret)
		return ret;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);
		const bool in_use =
			(UBI_PEB_MAPPED == state || UBI_PEB_ERRONEOUS == state);
		const bool is_named = (0 != (named[pnum / BITS_PER_BYTE] &
					     BIT(pnum % BITS_PER_BYTE)));

		if (in_use && !is_named) {
			LOG_ERR("PEB %u is in use but backs no logical block",
				pnum);
			return -EFAULT;
		}
	}

	return 0;
}
