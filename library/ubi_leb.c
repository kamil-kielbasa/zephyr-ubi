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
 * \brief Check that the header of the block behind a logical block verifies
 *        and names that logical block in this image.
 *
 * \retval 0
 *         It does.
 * \retval -EBADMSG
 *         It does not verify, or names something else.
 * \retval -EIO
 *         The flash driver failed.
 */
static int leb_header_verify(const struct ubi_device *ubi, uint16_t pnum,
			     uint32_t vol_id, uint32_t lnum);

/**
 * \brief Put the contents on a block of their own and move the mapping there.
 *
 *        The header goes down first and carries a length and a checksum, so
 *        an interruption loses to whatever held the mapping before.
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

static int leb_header_verify(const struct ubi_device *ubi, uint16_t pnum,
			     uint32_t vol_id, uint32_t lnum)
{
	struct ubi_headers headers = { 0 };
	const int ret = ubi_impl_header_read(ubi, pnum, &headers);

	if (0 != ret)
		return ret;

	if (UBI_HEADER_OK != headers.vid_status) {
		LOG_ERR("PEB %u: holds volume %u block %u and its header no "
			"longer verifies (%d)",
			pnum, vol_id, lnum, headers.vid_status);
		return -EBADMSG;
	}

	/* An older authentic header can be put back in its place. */
	if (vol_id != headers.vid.vol_id || lnum != headers.vid.lnum ||
	    ubi->image_seq != headers.vid.image_seq) {
		LOG_ERR("PEB %u: holds volume %u block %u but its header names "
			"volume %u block %u of image 0x%08x",
			pnum, vol_id, lnum, headers.vid.vol_id,
			headers.vid.lnum, headers.vid.image_seq);
		return -EBADMSG;
	}

	return 0;
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
		.sqnum = ubi_impl_sqnum_next(ubi),
		.vol_id = vol_id,
		.lnum = lnum,
		.image_seq = ubi->image_seq,
		.data_size = (uint32_t)length,
		.data_crc = sealed ? crc32_ieee(buffer, length) : 0,
		.copy_flag = sealed,
	};

	ret = ubi_impl_header_vid_write(ubi, pnum, &vid);

	/* The mapping has not moved. The block is retired rather than handed
	 * back, or the next allocation would pick it and fail the same way. */
	if (0 != ret) {
		ubi_impl_peb_retire(ubi, pnum, vol_id, lnum);
		return ret;
	}

	/* From here on the header is down, and the next attach would take
	 * whatever follows it for the block's contents. */
	if (0 != length) {
		ret = ubi_impl_io_write_data(ubi, pnum, 0, buffer, length);

		if (0 != ret) {
			ubi_impl_peb_withdraw(ubi, pnum, vol_id, lnum);
			return ret;
		}
	}

	ret = ubi_impl_volume_leb_set(ubi, vol_id, lnum, (uint16_t)pnum);

	if (0 != ret) {
		ubi_impl_peb_withdraw(ubi, pnum, vol_id, lnum);
		return ret;
	}

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

	if (UBI_LEB_UNMAPPED != previous)
		ubi_impl_peb_state_set(ubi, previous, UBI_PEB_RECLAIM);

	return 0;
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

	/* Queued, not erased: until reclaim runs the contents stay readable
	 * from raw flash. */
	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNMAPPED);

	return 0;
}

int ubi_impl_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	/* The copies waiting for reclaim go first, one an unmap left
	 * included, so a cut short can bring back only the newest. */
	ret = ubi_impl_peb_purge(ubi, vol_id, lnum, 1);

	if (0 != ret)
		return ret;

	if (UBI_LEB_UNMAPPED == pnum)
		return 0;

	ret = ubi_impl_volume_leb_set(ubi, vol_id, lnum, UBI_LEB_UNMAPPED);

	if (0 != ret)
		return ret;

	ret = ubi_impl_peb_prepare(ubi, pnum);

	if (0 != ret)
		ubi_impl_peb_retire(ubi, pnum, vol_id, lnum);

	return ret;
}

int ubi_impl_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		      uint32_t offset, uint8_t *buffer, size_t length)
{
	uint16_t pnum = UBI_LEB_UNMAPPED;
	int ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &pnum);

	if (0 != ret)
		return ret;

	const bool fits = leb_range_fits(ubi, offset, length);

	if (!fits) {
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

	if (0 == length)
		return 0;

	if (IS_ENABLED(CONFIG_UBI_VERIFY_ON_READ)) {
		ret = leb_header_verify(ubi, pnum, vol_id, lnum);

		if (0 != ret)
			return ret;
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

	const bool fits = leb_range_fits(ubi, 0, length);

	if (!fits) {
		LOG_ERR("volume %u block %u: %zu bytes do not fit in the %u a "
			"block holds",
			vol_id, lnum, length, ubi->geometry.leb_size);
		return -EINVAL;
	}

	const bool aligned = leb_range_aligned(ubi, 0, length);

	if (!aligned) {
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

	const bool fits = leb_range_fits(ubi, offset, length);

	if (!fits) {
		LOG_ERR("volume %u block %u: writing %zu bytes at %u runs past "
			"the %u a block holds",
			vol_id, lnum, length, offset, ubi->geometry.leb_size);
		return -EINVAL;
	}

	/* The caller owns the offset, so a partial write block would decide
	 * where the next one may start. */
	const bool aligned = leb_range_aligned(ubi, offset, length);

	if (!aligned) {
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
