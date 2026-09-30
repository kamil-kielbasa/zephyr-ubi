/**
 * \file    ubi_relocate.c
 * \author  Kamil Kielbasa
 * \brief   Moving data off little-worn blocks so that they take their share
 *          of the erases.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_relocate.h"
#include "ubi_state.h"
#include "ubi_volume.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

BUILD_ASSERT(0 == CONFIG_UBI_IO_CHUNK_SIZE % UBI_HEADER_SIZE,
	     "a relocation copies whole write blocks at a time");

/* Static function declarations -------------------------------------------- */

/**
 * \brief Erase count of the block relocation would move onto, or zero when
 *        none is free.
 */
static uint32_t relocate_target_wear(const struct ubi_device *ubi);

/**
 * \brief Report whether a block is in use, unprotected, and worn far enough
 *        below \p target to be worth moving off.
 */
static bool relocate_worthwhile(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t target);

/**
 * \brief Check that no candidate is less worn than the one chosen.
 *
 * \retval 0
 *         The choice holds.
 * \retval -EFAULT
 *         It does not.
 */
static int relocate_check_choice(const struct ubi_device *ubi, uint32_t chosen,
				 uint32_t target);

/**
 * \brief Choose the least worn block worth moving off.
 *
 * \retval 0
 *         Chosen.
 * \retval -ENOENT
 *         None is.
 */
static int relocate_choose(const struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Bytes of a block's data area worth carrying over: all of it up to
 *        the last byte that is not erased, rounded up to a write block.
 *
 *        The whole area is read, whatever the header says, because an append
 *        may have gone past the seal.
 */
static int relocate_data_length(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t *length);

/**
 * \brief Checksum the first \p length bytes of a block's data area.
 */
static int relocate_data_crc(const struct ubi_device *ubi, uint32_t pnum,
			     uint32_t length, uint32_t *crc);

/**
 * \brief Copy a block's data area to another block, checksumming what was
 *        read on the way.
 *
 * \param[out] source_failed            Set when the source could not be
 *                                      read, as opposed to the target
 *                                      written.
 */
static int relocate_data_copy(struct ubi_device *ubi, uint32_t from,
			      uint32_t to, uint32_t length, uint32_t *crc,
			      bool *source_failed);

/**
 * \brief Check that the target reads back as the header it was given.
 */
static int relocate_target_verify(const struct ubi_device *ubi, uint32_t target,
				  const struct ubi_vid_header *vid);

/**
 * \brief Leave a block that could not be read or verified where it is, and
 *        never choose it again.
 */
static void relocate_refuse(struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Read and verify the block to move, and seal afresh what it holds.
 *
 *        The header is verified whatever CONFIG_UBI_VERIFY_ON_READ says:
 *        moving a block that does not verify would turn tampering into a
 *        fresh seal that does.
 *
 * \param[out] vid                      Header for the target.
 */
static int relocate_source_read(struct ubi_device *ubi, uint32_t source,
				struct ubi_vid_header *vid);

/**
 * \brief Write the target, check it reads back and map it in place of the
 *        source.
 */
static int relocate_copy(struct ubi_device *ubi, uint32_t source,
			 uint32_t target, const struct ubi_vid_header *vid);

/* Static function definitions --------------------------------------------- */

static uint32_t relocate_target_wear(const struct ubi_device *ubi)
{
	uint32_t target = 0;
	const int ret = ubi_impl_peb_allocate_for_levelling(ubi, &target);

	if (0 != ret)
		return 0;

	return ubi->blocks.erase_count[target];
}

static bool relocate_worthwhile(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t target)
{
	const enum ubi_peb_state state = ubi_impl_peb_state_get(ubi, pnum);

	if (UBI_PEB_MAPPED != state)
		return false;

	if (0 != ubi->blocks.protect[pnum])
		return false;

	/* Erase counts are unsigned and the target may be zero. */
	return ubi->blocks.erase_count[pnum] +
		       CONFIG_UBI_WEAR_LEVELING_THRESHOLD <
	       target;
}

static int relocate_check_choice(const struct ubi_device *ubi, uint32_t chosen,
				 uint32_t target)
{
	const uint32_t taken = ubi->blocks.erase_count[chosen];
	const bool chosen_worthwhile = relocate_worthwhile(ubi, chosen, target);

	if (!chosen_worthwhile) {
		LOG_ERR("PEB %u was not worth moving off", chosen);
		return -EFAULT;
	}

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const uint32_t count = ubi->blocks.erase_count[pnum];
		const bool worthwhile = relocate_worthwhile(ubi, pnum, target);

		if (worthwhile && count < taken) {
			LOG_ERR("PEB %u erased %u times was moved while PEB %u "
				"stands at %u",
				chosen, taken, pnum, count);
			return -EFAULT;
		}
	}

	return 0;
}

static int relocate_choose(const struct ubi_device *ubi, uint32_t *pnum)
{
	const uint32_t target = relocate_target_wear(ubi);
	uint32_t lowest = UINT32_MAX;
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		const bool worthwhile =
			relocate_worthwhile(ubi, candidate, target);

		if (!worthwhile)
			continue;

		if (found && ubi->blocks.erase_count[candidate] >= lowest)
			continue;

		lowest = ubi->blocks.erase_count[candidate];
		*pnum = candidate;
		found = true;
	}

	if (!found)
		return -ENOENT;

	if (IS_ENABLED(CONFIG_UBI_SELF_CHECKS))
		return relocate_check_choice(ubi, *pnum, target);

	return 0;
}

static int relocate_data_length(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t *length)
{
	uint8_t chunk[CONFIG_UBI_IO_CHUNK_SIZE];
	uint32_t end = ubi->geometry.leb_size;

	while (0 != end) {
		const uint32_t size = MIN(sizeof(chunk), end);
		const uint32_t at = end - size;
		const int ret =
			ubi_impl_io_read_data(ubi, pnum, at, chunk, size);

		if (0 != ret)
			return ret;

		uint32_t kept = size;

		while (0 != kept &&
		       ubi->geometry.erase_value == chunk[kept - 1])
			kept -= 1;

		if (0 != kept) {
			*length = ROUND_UP(at + kept,
					   ubi->geometry.write_block_size);
			return 0;
		}

		end = at;
	}

	*length = 0;

	return 0;
}

static int relocate_data_crc(const struct ubi_device *ubi, uint32_t pnum,
			     uint32_t length, uint32_t *crc)
{
	uint8_t chunk[CONFIG_UBI_IO_CHUNK_SIZE];
	uint32_t running = 0;

	for (uint32_t at = 0; at < length; at += sizeof(chunk)) {
		const uint32_t size = MIN(sizeof(chunk), length - at);
		const int ret =
			ubi_impl_io_read_data(ubi, pnum, at, chunk, size);

		if (0 != ret)
			return ret;

		running = crc32_ieee_update(running, chunk, size);
	}

	*crc = running;

	return 0;
}

static int relocate_data_copy(struct ubi_device *ubi, uint32_t from,
			      uint32_t to, uint32_t length, uint32_t *crc,
			      bool *source_failed)
{
	uint8_t chunk[CONFIG_UBI_IO_CHUNK_SIZE];
	uint32_t running = 0;

	*source_failed = false;

	for (uint32_t at = 0; at < length; at += sizeof(chunk)) {
		const uint32_t size = MIN(sizeof(chunk), length - at);
		int ret = ubi_impl_io_read_data(ubi, from, at, chunk, size);

		if (0 != ret) {
			*source_failed = true;
			return ret;
		}

		running = crc32_ieee_update(running, chunk, size);

		ret = ubi_impl_io_write_data(ubi, to, at, chunk, size);

		if (0 != ret)
			return ret;
	}

	*crc = running;

	return 0;
}

static int relocate_target_verify(const struct ubi_device *ubi, uint32_t target,
				  const struct ubi_vid_header *vid)
{
	struct ubi_headers headers = { 0 };
	uint32_t crc = 0;
	int ret = ubi_impl_header_read(ubi, target, &headers);

	if (0 != ret)
		return ret;

	if (UBI_HEADER_OK != headers.vid_status ||
	    vid->sqnum != headers.vid.sqnum ||
	    vid->vol_id != headers.vid.vol_id ||
	    vid->lnum != headers.vid.lnum ||
	    vid->data_size != headers.vid.data_size ||
	    vid->data_crc != headers.vid.data_crc) {
		LOG_ERR("PEB %u: the header written to it does not read back",
			target);
		return -EIO;
	}

	ret = relocate_data_crc(ubi, target, vid->data_size, &crc);

	if (0 != ret)
		return ret;

	if (crc != vid->data_crc) {
		LOG_ERR("PEB %u: the data written to it does not read back",
			target);
		return -EIO;
	}

	return 0;
}

static void relocate_refuse(struct ubi_device *ubi, uint32_t pnum)
{
	LOG_ERR("PEB %u: cannot be read or verified, so it stays where it is "
		"and is not moved again",
		pnum);

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_ERRONEOUS);
}

static int relocate_source_read(struct ubi_device *ubi, uint32_t source,
				struct ubi_vid_header *vid)
{
	struct ubi_headers headers = { 0 };
	uint16_t mapped = UBI_LEB_UNMAPPED;
	uint32_t length = 0;
	uint32_t crc = 0;
	int ret = ubi_impl_header_read(ubi, source, &headers);

	if (0 != ret) {
		relocate_refuse(ubi, source);
		return ret;
	}

	if (UBI_HEADER_OK != headers.vid_status) {
		const enum ubi_event_type event =
			(UBI_HEADER_TAMPERED == headers.vid_status) ?
				UBI_EVENT_HDR_TAMPERED :
				UBI_EVENT_HDR_CORRUPT;

		ubi_impl_event_emit(ubi, event, source, UBI_VOL_ID_INVALID, 0);
		relocate_refuse(ubi, source);
		return -EBADMSG;
	}

	ret = ubi_impl_volume_leb_get(ubi, headers.vid.vol_id, headers.vid.lnum,
				      &mapped);

	if (0 != ret || mapped != source) {
		LOG_ERR("PEB %u: claims volume %u block %u, which PEB %u backs",
			source, headers.vid.vol_id, headers.vid.lnum, mapped);
		ubi_impl_peb_state_set(ubi, source, UBI_PEB_RECLAIM);
		return -EFAULT;
	}

	ret = relocate_data_length(ubi, source, &length);

	if (0 != ret) {
		relocate_refuse(ubi, source);
		return ret;
	}

	ret = relocate_data_crc(ubi, source, length, &crc);

	if (0 != ret) {
		relocate_refuse(ubi, source);
		return ret;
	}

	*vid = (struct ubi_vid_header){
		.sqnum = ubi_impl_sqnum_next(ubi),
		.vol_id = headers.vid.vol_id,
		.lnum = headers.vid.lnum,
		.image_seq = ubi->image_seq,
		.data_size = length,
		.data_crc = crc,
		.copy_flag = (0 != length),
	};

	return 0;
}

static int relocate_copy(struct ubi_device *ubi, uint32_t source,
			 uint32_t target, const struct ubi_vid_header *vid)
{
	uint32_t copied = 0;
	bool source_failed = false;
	int ret = ubi_impl_header_vid_write(ubi, target, vid);

	if (0 != ret) {
		ubi_impl_peb_retire(ubi, target, vid->vol_id, vid->lnum);
		return ret;
	}

	ret = relocate_data_copy(ubi, source, target, vid->data_size, &copied,
				 &source_failed);

	if (0 != ret && source_failed) {
		relocate_refuse(ubi, source);
		ubi_impl_peb_state_set(ubi, target, UBI_PEB_RECLAIM);
		return ret;
	}

	/* Its header is down: once the source is gone the next attach would
	 * take whatever followed it for the block's contents. */
	if (0 != ret) {
		ubi_impl_peb_withdraw(ubi, target, vid->vol_id, vid->lnum);
		return ret;
	}

	if (copied != vid->data_crc) {
		LOG_ERR("PEB %u: reads back differently each time", source);
		relocate_refuse(ubi, source);
		ubi_impl_peb_state_set(ubi, target, UBI_PEB_RECLAIM);
		return -EBADMSG;
	}

	ret = relocate_target_verify(ubi, target, vid);

	if (0 != ret) {
		ubi_impl_peb_withdraw(ubi, target, vid->vol_id, vid->lnum);
		return ret;
	}

	ret = ubi_impl_volume_leb_set(ubi, vid->vol_id, vid->lnum,
				      (uint16_t)target);

	if (0 != ret) {
		ubi_impl_peb_state_set(ubi, target, UBI_PEB_RECLAIM);
		return ret;
	}

	ubi_impl_peb_state_set(ubi, target, UBI_PEB_MAPPED);

	return 0;
}

/* Module interface function definitions ----------------------------------- */

uint32_t ubi_impl_relocate_pending(const struct ubi_device *ubi)
{
	const uint32_t target = relocate_target_wear(ubi);
	uint32_t pending = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const bool worthwhile = relocate_worthwhile(ubi, pnum, target);

		if (worthwhile)
			pending += 1;
	}

	return pending;
}

int ubi_impl_relocate_step(struct ubi_device *ubi)
{
	struct ubi_vid_header vid = { 0 };
	uint32_t source = 0;
	uint32_t target = 0;
	int ret = relocate_choose(ubi, &source);

	if (0 != ret)
		return ret;

	ret = ubi_impl_peb_allocate_for_levelling(ubi, &target);

	if (0 != ret)
		return ret;

	ret = relocate_source_read(ubi, source, &vid);

	if (0 != ret)
		return ret;

	ret = relocate_copy(ubi, source, target, &vid);

	if (0 != ret)
		return ret;

	LOG_INF("volume %u block %u moved from PEB %u to PEB %u, %u bytes",
		vid.vol_id, vid.lnum, source, target, vid.data_size);

	/* Erased in the same step, so that no second copy outlives the move.
	 * The move stands either way. */
	ret = ubi_impl_peb_prepare(ubi, source);

	if (0 != ret)
		ubi_impl_peb_retire(ubi, source, UBI_VOL_ID_INVALID, 0);

	return 0;
}
