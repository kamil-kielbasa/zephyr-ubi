/**
 * \file    ubi_leb.c
 * \author  Kamil Kielbasa
 * \brief   What a logical erase block can be asked to do.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_leb.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_volume.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Report whether a range stays inside one logical erase block.
 */
static bool leb_range_fits(const struct ubi_device *ubi, uint32_t offset,
			   size_t length);

/**
 * \brief Report whether a range covers whole write blocks.
 */
static bool leb_range_aligned(const struct ubi_device *ubi, uint32_t offset,
			      size_t length);

/**
 * \brief Put the contents on a block of their own and move the mapping there.
 *
 *        The header goes down before the data and carries a length and a
 *        checksum over it, so an interruption is recognisable at the next
 *        attach and the block loses to whatever held the mapping before.
 */
static int leb_claim(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint16_t previous, const uint8_t *buffer, size_t length,
		     bool sealed);

/* Static function definitions --------------------------------------------- */

static bool leb_range_fits(const struct ubi_device *ubi, uint32_t offset,
			   size_t length)
{
	if (offset > ubi->geometry.leb_size)
		return false;

	if (length > ubi->geometry.leb_size - offset)
		return false;

	return true;
}

static bool leb_range_aligned(const struct ubi_device *ubi, uint32_t offset,
			      size_t length)
{
	if (0 != offset % ubi->geometry.write_block_size)
		return false;

	if (0 != length % ubi->geometry.write_block_size)
		return false;

	return true;
}

static int leb_claim(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint16_t previous, const uint8_t *buffer, size_t length,
		     bool sealed)
{
	uint32_t pnum = 0;
	int ret = ubi_impl_peb_allocate_for_write(ubi, &pnum);

	if (0 != ret)
		return ret;

	const struct ubi_vid_header vid = {
		.sqnum = ubi->global_sqnum + 1,
		.vol_id = vol_id,
		.lnum = lnum,
		.image_seq = ubi->image_seq,
		.data_size = (uint32_t)length,
		.data_crc = sealed ? crc32_ieee(buffer, length) : 0,
		.copy_flag = sealed,
	};

	ret = ubi_impl_header_vid_write(ubi, pnum, &vid);

	if (0 != ret)
		goto give_back;

	if (0 != length) {
		ret = ubi_impl_io_write_data(ubi, pnum, 0, buffer, length);

		if (0 != ret)
			goto give_back;
	}

	ret = ubi_impl_volume_leb_set(ubi, vol_id, lnum, (uint16_t)pnum);

	if (0 != ret)
		goto give_back;

	ubi->global_sqnum = vid.sqnum;
	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

	if (UBI_LEB_UNMAPPED != previous)
		ubi_impl_peb_state_set(ubi, previous, UBI_PEB_RECLAIM);

	return 0;

give_back:
	/* The mapping has not moved, so the logical block is intact. The
	 * physical one is retired rather than queued: handing it back would
	 * let the next allocation pick it and fail the same way. */
	ubi_impl_peb_retire(ubi, pnum, vol_id, lnum);

	return ret;
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	const int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	if (UBI_LEB_UNMAPPED != pnum) {
		LOG_ERR("volume %u block %u already has PEB %u behind it",
			vol_id, lnum, pnum);
		return -EEXIST;
	}

	return leb_claim(ubi, vol_id, lnum, pnum, NULL, 0, false);
}

int ubi_impl_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	if (UBI_LEB_UNMAPPED == pnum)
		return 0;

	ret = ubi_impl_volume_leb_set(ubi, vol_id, lnum, UBI_LEB_UNMAPPED);

	if (0 != ret)
		return ret;

	/* Queued, not erased: the erase is the application's call to make,
	 * so until then the contents stay readable from raw flash. */
	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);

	return 0;
}

int ubi_impl_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	if (UBI_LEB_UNMAPPED == pnum)
		return 0;

	ret = ubi_impl_volume_leb_set(ubi, vol_id, lnum, UBI_LEB_UNMAPPED);

	if (0 != ret)
		return ret;

	ret = ubi_impl_peb_prepare(ubi, pnum);

	if (0 != ret) {
		ubi_impl_peb_retire(ubi, pnum, vol_id, lnum);
		return ret;
	}

	return 0;
}

int ubi_impl_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		      uint32_t offset, uint8_t *buffer, size_t length)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	const int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	if (!leb_range_fits(ubi, offset, length)) {
		LOG_ERR("volume %u block %u: reading %zu bytes at %u runs past "
			"the %u a block holds",
			vol_id, lnum, length, offset, ubi->geometry.leb_size);
		return -EINVAL;
	}

	/* Nothing behind it reads the same as a block nobody has written. */
	if (UBI_LEB_UNMAPPED == pnum) {
		memset(buffer, ubi->geometry.erase_value, length);
		return 0;
	}

	if (IS_ENABLED(CONFIG_UBI_VERIFY_ON_READ)) {
		struct ubi_headers headers = { 0 };
		const int verified = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != verified)
			return verified;

		if (UBI_HEADER_OK != headers.vid_status) {
			LOG_ERR("PEB %u: holds volume %u block %u and its "
				"header no longer verifies (%d)",
				pnum, vol_id, lnum, headers.vid_status);
			return -EBADMSG;
		}
	}

	return ubi_impl_io_read_data(ubi, pnum, offset, buffer, length);
}

int ubi_impl_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
			const uint8_t *buffer, size_t length)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	const int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	if (!leb_range_fits(ubi, 0, length)) {
		LOG_ERR("volume %u block %u: %zu bytes do not fit in the %u a "
			"block holds",
			vol_id, lnum, length, ubi->geometry.leb_size);
		return -EINVAL;
	}

	if (!leb_range_aligned(ubi, 0, length)) {
		LOG_ERR("volume %u block %u: %zu bytes are not whole write "
			"blocks of %u",
			vol_id, lnum, length, ubi->geometry.write_block_size);
		return -EINVAL;
	}

	return leb_claim(ubi, vol_id, lnum, pnum, buffer, length, true);
}

int ubi_impl_leb_write_at(struct ubi_device *ubi, uint32_t vol_id,
			  uint32_t lnum, uint32_t offset, const uint8_t *buffer,
			  size_t length)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	if (!leb_range_fits(ubi, offset, length)) {
		LOG_ERR("volume %u block %u: writing %zu bytes at %u runs past "
			"the %u a block holds",
			vol_id, lnum, length, offset, ubi->geometry.leb_size);
		return -EINVAL;
	}

	/* The caller owns the offset, so a partial write block would decide
	 * where the next one may start. */
	if (!leb_range_aligned(ubi, offset, length)) {
		LOG_ERR("volume %u block %u: %zu bytes at %u are not whole "
			"write blocks of %u",
			vol_id, lnum, length, offset,
			ubi->geometry.write_block_size);
		return -EINVAL;
	}

	if (UBI_LEB_UNMAPPED == pnum) {
		ret = leb_claim(ubi, vol_id, lnum, pnum, NULL, 0, false);

		if (0 != ret)
			return ret;

		ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

		if (0 != ret)
			return ret;
	}

	return ubi_impl_io_write_data(ubi, pnum, offset, buffer, length);
}

int ubi_impl_leb_get_info(struct ubi_device *ubi, uint32_t vol_id,
			  uint32_t lnum, struct ubi_leb_info *info)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	const int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	const bool mapped = (UBI_LEB_UNMAPPED != pnum);
	const struct ubi_leb_info described = {
		.mapped = mapped,
		.erase_count = mapped ? ubi->blocks.erase_count[pnum] : 0,
	};

	*info = described;

	return 0;
}
