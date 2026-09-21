/**
 * \file    ubi_maintenance.c
 * \author  Kamil Kielbasa
 * \brief   Work the device puts off until the application has time for it.
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
#include <ubi/ubi.h>

#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_maintenance.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_volume.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Bytes read at a time when copying or checksumming a block's data area. */
#define RELOCATE_CHUNK (128)

/* Static function declarations -------------------------------------------- */

/** \name Erasing what a logical block let go */
/**@{*/

/**
 * \brief Count the blocks waiting to be erased.
 */
static uint32_t reclaim_pending(const struct ubi_device *ubi);

/**
 * \brief Choose one to erase.
 *
 *        Blocks released by a logical block go first: they still carry the
 *        application's data, so the sooner they are erased the shorter that
 *        data stays readable from raw flash.
 *
 * \retval 0
 *         Chosen.
 * \retval -ENOENT
 *         Nothing is waiting.
 */
static int reclaim_choose(const struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Erase and stamp it.
 */
static int reclaim_step(struct ubi_device *ubi);

/**@}*/

/** \name Spreading the wear */
/**@{*/

/**
 * \brief Erase count of the block relocation would move onto, or zero when
 *        none is free.
 */
static uint32_t relocate_target_wear(const struct ubi_device *ubi);

/**
 * \brief Report whether a block's wear is far enough below that target to be
 *        worth moving off.
 */
static bool relocate_worthwhile(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t target);

/**
 * \brief Count the blocks worth moving off.
 */
static uint32_t relocate_pending(const struct ubi_device *ubi);

/**
 * \brief Choose the least worn of them, which is the one with the most life
 *        left to hand back to the traffic that keeps changing.
 *
 * \retval 0
 *         Chosen.
 * \retval -ENOENT
 *         The device is evenly worn.
 */
static int relocate_choose(const struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Say the other way round that no candidate has more life left than
 *        the one relocation took.
 *
 * \retval 0
 *         The choice holds.
 * \retval -EFAULT
 *         It does not, and the library has contradicted itself.
 */
static int relocate_check_choice(const struct ubi_device *ubi, uint32_t chosen,
				 uint32_t target);

/**
 * \brief Bytes of a block's data area worth carrying to the new block.
 *
 *        Trailing erased bytes are dropped and the result is rounded up to a
 *        whole write block. Dropping them cannot lose anything, because the
 *        target block is erased and reads those positions back the same way;
 *        what it buys is that the seal placed on the new block stops short of
 *        the space an append may still want.
 */
static int relocate_data_length(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t from, uint32_t *length);

/**
 * \brief Checksum a block's data area.
 */
static int relocate_data_crc(const struct ubi_device *ubi, uint32_t pnum,
			     uint32_t length, uint32_t *crc);

/**
 * \brief Copy a block's data area to another block.
 */
static int relocate_data_copy(struct ubi_device *ubi, uint32_t from,
			      uint32_t to, uint32_t length);

/**
 * \brief Move the contents of one block to another and switch the mapping.
 */
static int relocate_step(struct ubi_device *ubi);

/**@}*/

/** \name Undoing damage */
/**@{*/

/**
 * \brief Count what is waiting to be put right.
 */
static uint32_t repair_pending(const struct ubi_device *ubi);

/**
 * \brief Find the first block still waiting for another chance.
 *
 *        Blocks already written off are skipped: they had theirs.
 *
 * \retval 0
 *         Found.
 * \retval -ENOENT
 *         Every block is in use, waiting, or finished.
 */
static int repair_choose(const struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Put one thing right: the volume table first, then a retired block.
 */
static int repair_step(struct ubi_device *ubi);

/**@}*/

/** \name Dispatch */
/**@{*/

/**
 * \brief Count the work of one kind that is still waiting.
 */
static int maintenance_pending(const struct ubi_device *ubi,
			       enum ubi_maintenance_op operation,
			       uint32_t *pending);

/**
 * \brief Carry out one unit of work of one kind.
 */
static int maintenance_step(struct ubi_device *ubi,
			    enum ubi_maintenance_op operation);

/**@}*/

/* Static function definitions --------------------------------------------- */

static uint32_t reclaim_pending(const struct ubi_device *ubi)
{
	uint32_t pending = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_RECLAIM == state || UBI_PEB_UNKNOWN == state)
			pending += 1;
	}

	return pending;
}

static int reclaim_choose(const struct ubi_device *ubi, uint32_t *pnum)
{
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, candidate);

		if (UBI_PEB_RECLAIM == state) {
			*pnum = candidate;
			return 0;
		}

		if (UBI_PEB_UNKNOWN == state && !found) {
			*pnum = candidate;
			found = true;
		}
	}

	return found ? 0 : -ENOENT;
}

static int reclaim_step(struct ubi_device *ubi)
{
	uint32_t pnum = 0;
	int ret = reclaim_choose(ubi, &pnum);

	if (0 != ret)
		return ret;

	ret = ubi_impl_peb_prepare(ubi, pnum);

	if (0 != ret) {
		ubi_impl_peb_retire(ubi, pnum, UBI_VOL_ID_INVALID, 0);
		return ret;
	}

	return 0;
}

static uint32_t relocate_target_wear(const struct ubi_device *ubi)
{
	uint32_t target = 0;

	if (0 != ubi_impl_peb_allocate_for_levelling(ubi, &target))
		return 0;

	return ubi->blocks.erase_count[target];
}

static bool relocate_worthwhile(const struct ubi_device *ubi, uint32_t pnum,
				uint32_t target)
{
	if (UBI_PEB_MAPPED != ubi_impl_peb_state_get(ubi, pnum))
		return false;

	if (0 != ubi->blocks.protect[pnum])
		return false;

	/* Written this way round because erase counts are unsigned and there
	 * may be no target at all. */
	return ubi->blocks.erase_count[pnum] +
		       CONFIG_UBI_WEAR_LEVELING_THRESHOLD <
	       target;
}

static int relocate_check_choice(const struct ubi_device *ubi, uint32_t chosen,
				 uint32_t target)
{
	const uint32_t taken = ubi->blocks.erase_count[chosen];

	if (!relocate_worthwhile(ubi, chosen, target)) {
		LOG_ERR("PEB %u was not worth moving off", chosen);
		return -EFAULT;
	}

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const uint32_t count = ubi->blocks.erase_count[pnum];

		if (!relocate_worthwhile(ubi, pnum, target))
			continue;

		if (count < taken) {
			LOG_ERR("PEB %u erased %u times was moved while PEB %u "
				"stands at %u with more life to give",
				chosen, taken, pnum, count);
			return -EFAULT;
		}
	}

	return 0;
}

static uint32_t relocate_pending(const struct ubi_device *ubi)
{
	const uint32_t target = relocate_target_wear(ubi);
	uint32_t pending = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		if (relocate_worthwhile(ubi, pnum, target))
			pending += 1;
	}

	return pending;
}

static int relocate_choose(const struct ubi_device *ubi, uint32_t *pnum)
{
	const uint32_t target = relocate_target_wear(ubi);
	uint32_t lowest = UINT32_MAX;
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		if (!relocate_worthwhile(ubi, candidate, target))
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
				uint32_t from, uint32_t *length)
{
	uint8_t chunk[RELOCATE_CHUNK];
	uint32_t end = from;

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
	uint8_t chunk[RELOCATE_CHUNK];
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
			      uint32_t to, uint32_t length)
{
	uint8_t chunk[RELOCATE_CHUNK];

	for (uint32_t at = 0; at < length; at += sizeof(chunk)) {
		const uint32_t size = MIN(sizeof(chunk), length - at);
		int ret = ubi_impl_io_read_data(ubi, from, at, chunk, size);

		if (0 != ret)
			return ret;

		ret = ubi_impl_io_write_data(ubi, to, at, chunk, size);

		if (0 != ret)
			return ret;
	}

	return 0;
}

static int relocate_step(struct ubi_device *ubi)
{
	uint32_t source = 0;
	int ret = relocate_choose(ubi, &source);

	if (0 != ret)
		return ret;

	struct ubi_headers headers = { 0 };

	ret = ubi_impl_header_read(ubi, source, &headers);

	if (0 != ret)
		return ret;

	/* The MAC is checked here whatever CONFIG_UBI_VERIFY_ON_READ says:
	 * moving a block that does not verify would launder tampering into a
	 * fresh seal that does. */
	if (UBI_HEADER_OK != headers.vid_status) {
		ubi_impl_peb_retire(ubi, source, UBI_VOL_ID_INVALID, 0);
		return -EBADMSG;
	}

	const uint32_t vol_id = headers.vid.vol_id;
	const uint32_t lnum = headers.vid.lnum;
	const bool sealed = headers.vid.copy_flag;
	uint16_t mapped = UBI_LEB_UNMAPPED;
	uint32_t length = 0;

	ret = ubi_impl_volume_leb_get(ubi, vol_id, lnum, &mapped);

	if (0 != ret) {
		LOG_WRN("PEB %u: claims volume %u block %u, which no longer "
			"exists; queued for reclaim",
			source, vol_id, lnum);
		ubi_impl_peb_state_set(ubi, source, UBI_PEB_RECLAIM);
		return ret;
	}

	/* An unsealed block promised no length, so the whole area is fair
	 * game and the trailing erased bytes are what bound the copy. */
	const uint32_t scan_from = sealed ? headers.vid.data_size :
					    ubi->geometry.leb_size;

	ret = relocate_data_length(ubi, source, scan_from, &length);

	if (0 != ret)
		return ret;

	uint32_t crc = 0;

	ret = relocate_data_crc(ubi, source, length, &crc);

	if (0 != ret)
		return ret;

	/* A sealed block promised a checksum, so carrying its data anywhere
	 * without honouring that promise would re-seal whatever rot had
	 * reached it in the meantime. */
	if (sealed && length == headers.vid.data_size &&
	    crc != headers.vid.data_crc) {
		LOG_ERR("PEB %u: holds volume %u block %u and its data no "
			"longer matches the checksum it carries",
			source, vol_id, lnum);
		ubi_impl_peb_retire(ubi, source, vol_id, lnum);
		return -EBADMSG;
	}

	uint32_t target = 0;

	ret = ubi_impl_peb_allocate_for_levelling(ubi, &target);

	if (0 != ret)
		return ret;

	const struct ubi_vid_header vid = {
		.sqnum = ubi->global_sqnum + 1,
		.vol_id = vol_id,
		.lnum = lnum,
		.image_seq = ubi->image_seq,
		.data_size = length,
		.data_crc = crc,
		.copy_flag = true,
	};

	ret = ubi_impl_header_vid_write(ubi, target, &vid);

	if (0 != ret) {
		ubi_impl_peb_retire(ubi, target, vol_id, lnum);
		return ret;
	}

	ret = relocate_data_copy(ubi, source, target, length);

	if (0 != ret) {
		ubi_impl_peb_retire(ubi, target, vol_id, lnum);
		return ret;
	}

	ret = ubi_impl_volume_leb_set(ubi, vol_id, lnum, (uint16_t)target);

	if (0 != ret) {
		ubi_impl_peb_state_set(ubi, target, UBI_PEB_UNKNOWN);
		return ret;
	}

	ubi->global_sqnum = vid.sqnum;
	ubi_impl_peb_state_set(ubi, target, UBI_PEB_MAPPED);
	ubi_impl_peb_state_set(ubi, source, UBI_PEB_RECLAIM);

	LOG_INF("volume %u block %u moved from PEB %u to PEB %u, %u bytes",
		vol_id, lnum, source, target, length);

	return 0;
}

static uint32_t repair_pending(const struct ubi_device *ubi)
{
	uint32_t pending = ubi->volume_table.degraded ? 1 : 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		if (UBI_PEB_BAD == ubi_impl_peb_state_get(ubi, pnum))
			pending += 1;
	}

	return pending;
}

static int repair_choose(const struct ubi_device *ubi, uint32_t *pnum)
{
	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		if (UBI_PEB_BAD != ubi_impl_peb_state_get(ubi, candidate))
			continue;

		*pnum = candidate;

		return 0;
	}

	return -ENOENT;
}

static int repair_step(struct ubi_device *ubi)
{
	if (ubi->volume_table.degraded)
		return ubi_impl_volumes_rewrite(ubi);

	uint32_t pnum = 0;
	int ret = repair_choose(ubi, &pnum);

	if (0 != ret)
		return ret;

	/*
	 * Retirement lives in RAM, so a block put aside after one bad write
	 * deserves a second look without waiting for the next attach.
	 */
	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);

	ret = ubi_impl_peb_prepare(ubi, pnum);

	if (0 != ret) {
		/* Finding out that a block is finished is an answer, not a
		 * failure of the repair: it is written off so that the next
		 * step reaches the blocks that may still come back. */
		ubi_impl_peb_write_off(ubi, pnum);
	}

	return 0;
}

static int maintenance_pending(const struct ubi_device *ubi,
			       enum ubi_maintenance_op operation,
			       uint32_t *pending)
{
	switch (operation) {
	case UBI_MAINTENANCE_RECLAIM:
		*pending = reclaim_pending(ubi);
		return 0;
	case UBI_MAINTENANCE_RELOCATE:
		*pending = relocate_pending(ubi);
		return 0;
	case UBI_MAINTENANCE_REPAIR:
		*pending = repair_pending(ubi);
		return 0;
	default:
		return -EINVAL;
	}
}

static int maintenance_step(struct ubi_device *ubi,
			    enum ubi_maintenance_op operation)
{
	switch (operation) {
	case UBI_MAINTENANCE_RECLAIM:
		return reclaim_step(ubi);
	case UBI_MAINTENANCE_RELOCATE:
		return relocate_step(ubi);
	case UBI_MAINTENANCE_REPAIR:
		return repair_step(ubi);
	default:
		return -EINVAL;
	}
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_maintenance(struct ubi_device *ubi,
			 enum ubi_maintenance_op operation, uint32_t budget,
			 struct ubi_maintenance_result *result)
{
	uint32_t performed = 0;
	int ret = 0;

	for (uint32_t step = 0; step < budget && 0 == ret; ++step) {
		ret = maintenance_step(ubi, operation);

		if (0 == ret)
			performed += 1;
	}

	/* Running out of work is how a budget larger than the work ends, not
	 * something to report as a failure. */
	if (-ENOENT == ret)
		ret = 0;

	result->performed = performed;

	const int counted =
		maintenance_pending(ubi, operation, &result->remaining);

	if (0 != counted) {
		LOG_ERR("maintenance operation %d is not one this build "
			"performs",
			operation);
		return counted;
	}

	if (0 != ret) {
		LOG_ERR("maintenance operation %d stopped after %u of %u "
			"steps (%d)",
			operation, performed, budget, ret);
		return ret;
	}

	return 0;
}
