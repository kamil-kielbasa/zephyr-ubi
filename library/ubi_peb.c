/**
 * \file    ubi_peb.c
 * \author  Kamil Kielbasa
 * \brief   Lifecycle of a physical erase block.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_state.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Zeroes an invalidation writes over a magic: the widest write block a
 *  device is attached with, which divides a header. */
#define INVALIDATION_MAX_SIZE (UBI_HEADER_SIZE)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Bring every protection countdown one erase closer to expiring.
 *
 * \param pnum                          Block just erased, which holds nothing
 *                                      worth protecting.
 */
static void peb_protection_tick(struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Check that no block in \p state beats \p chosen, for
 *        \c CONFIG_UBI_SELF_CHECKS.
 *
 * \retval 0
 *         The choice holds.
 * \retval -EFAULT
 *         It does not.
 */
static int peb_check_extreme(const struct ubi_device *ubi,
			     enum ubi_peb_state state, uint32_t chosen,
			     bool lowest);

/**
 * \brief Check the same of a choice bounded by \p ceiling.
 *
 * \retval 0
 *         The choice holds.
 * \retval -EFAULT
 *         It does not.
 */
static int peb_check_within(const struct ubi_device *ubi, uint32_t chosen,
			    uint32_t ceiling);

/**
 * \brief Find the least worn block in a given state.
 */
static int peb_least_worn(const struct ubi_device *ubi,
			  enum ubi_peb_state state, uint32_t *pnum);

/**
 * \brief Find the most worn free block still within \p span erases of the
 *        least worn one, so that no single block absorbs every allocation.
 */
static int peb_free_most_worn_below(const struct ubi_device *ubi, uint32_t span,
				    uint32_t *pnum);

/**
 * \brief Erase a block for a write when none is free: the least worn blank
 *        or foreign one, and failing that one waiting for reclaim, so that
 *        running out of free blocks never depends on the application.
 *
 * \retval -ENOSPC
 *         Every block is in use or out of service.
 */
static int peb_erase_for_write(struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Clear the magic of every header that still verifies, the erase
 *        counter header first.
 *
 *        A block without valid headers is erased before use, so an erase
 *        cut short cannot bring stale contents back. A valid erase counter
 *        header over an invalid one behind it would read as damage instead.
 */
static int peb_invalidate(struct ubi_device *ubi, uint32_t pnum,
			  const struct ubi_headers *headers);

/**
 * \brief Leave an attached device read-only once a block stays unerased:
 *        what it held is still on it.
 *
 * \return \p ret.
 */
static int peb_erase_failed(struct ubi_device *ubi, uint32_t pnum, int ret);

/**
 * \brief Choose the next block \ref ubi_impl_peb_purge erases.
 *
 * \param[out] lnum                     Logical block its header names.
 *
 * \retval 0
 *         Chosen.
 * \retval -ENOENT
 *         No such block is left.
 * \retval -EIO
 *         A header could not be read.
 */
static int peb_purge_next(const struct ubi_device *ubi, uint32_t vol_id,
			  uint32_t first, uint32_t count, uint32_t *pnum,
			  uint32_t *lnum);

/* Static function definitions --------------------------------------------- */

static void peb_protection_tick(struct ubi_device *ubi, uint32_t pnum)
{
	for (uint32_t i = 0; i < ubi->geometry.peb_count; ++i) {
		if (0 != ubi->blocks.protect[i])
			ubi->blocks.protect[i] -= 1;
	}

	ubi->blocks.protect[pnum] = 0;
}

static int peb_check_extreme(const struct ubi_device *ubi,
			     enum ubi_peb_state state, uint32_t chosen,
			     bool lowest)
{
	const uint32_t taken = ubi->blocks.erase_count[chosen];
	const enum ubi_peb_state chosen_state =
		ubi_impl_peb_state_get(ubi, chosen);

	if (state != chosen_state) {
		LOG_ERR("PEB %u was chosen out of the wrong state", chosen);
		return -EFAULT;
	}

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const uint32_t count = ubi->blocks.erase_count[pnum];
		const enum ubi_peb_state other =
			ubi_impl_peb_state_get(ubi, pnum);

		if (state != other)
			continue;

		if (lowest ? count < taken : count > taken) {
			LOG_ERR("PEB %u erased %u times was chosen over PEB %u "
				"standing at %u",
				chosen, taken, pnum, count);
			return -EFAULT;
		}
	}

	return 0;
}

static int peb_check_within(const struct ubi_device *ubi, uint32_t chosen,
			    uint32_t ceiling)
{
	const uint32_t taken = ubi->blocks.erase_count[chosen];
	const enum ubi_peb_state chosen_state =
		ubi_impl_peb_state_get(ubi, chosen);

	if (UBI_PEB_FREE != chosen_state) {
		LOG_ERR("PEB %u was chosen out of the wrong state", chosen);
		return -EFAULT;
	}

	if (taken >= ceiling) {
		LOG_ERR("PEB %u stands at %u, past the %u it was bounded by",
			chosen, taken, ceiling);
		return -EFAULT;
	}

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const uint32_t count = ubi->blocks.erase_count[pnum];
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_FREE != state)
			continue;

		if (count < ceiling && count > taken) {
			LOG_ERR("PEB %u erased %u times was chosen over PEB %u "
				"standing at %u",
				chosen, taken, pnum, count);
			return -EFAULT;
		}
	}

	return 0;
}

static int peb_least_worn(const struct ubi_device *ubi,
			  enum ubi_peb_state state, uint32_t *pnum)
{
	uint32_t lowest = UINT32_MAX;
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		const enum ubi_peb_state candidate_state =
			ubi_impl_peb_state_get(ubi, candidate);

		if (state != candidate_state)
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
		return peb_check_extreme(ubi, state, *pnum, true);

	return 0;
}

static int peb_free_most_worn_below(const struct ubi_device *ubi, uint32_t span,
				    uint32_t *pnum)
{
	uint32_t least = 0;
	const int ret = peb_least_worn(ubi, UBI_PEB_FREE, &least);

	if (0 != ret)
		return ret;

	/* Erase counts stop at #UBI_MAX_ERASE_COUNT, so this cannot wrap. */
	const uint32_t ceiling = ubi->blocks.erase_count[least] + span;
	uint32_t highest = ubi->blocks.erase_count[least];

	*pnum = least;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		const uint32_t count = ubi->blocks.erase_count[candidate];
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, candidate);

		if (UBI_PEB_FREE != state)
			continue;

		if (count >= ceiling || count <= highest)
			continue;

		highest = count;
		*pnum = candidate;
	}

	if (IS_ENABLED(CONFIG_UBI_SELF_CHECKS))
		return peb_check_within(ubi, *pnum, ceiling);

	return 0;
}

static int peb_erase_for_write(struct ubi_device *ubi, uint32_t *pnum)
{
	static const enum ubi_peb_state order[] = {
		UBI_PEB_UNKNOWN,
		UBI_PEB_RECLAIM,
		UBI_PEB_UNMAPPED,
	};

	for (size_t i = 0; i < ARRAY_SIZE(order); ++i) {
		const int ret = peb_least_worn(ubi, order[i], pnum);

		if (-ENOENT == ret)
			continue;

		if (0 != ret)
			return ret;

		return ubi_impl_peb_reclaim(ubi, *pnum);
	}

	LOG_ERR("no block is free, blank or waiting for reclaim");

	return -ENOSPC;
}

static int peb_invalidate(struct ubi_device *ubi, uint32_t pnum,
			  const struct ubi_headers *headers)
{
	static const uint8_t zeroes[INVALIDATION_MAX_SIZE] = { 0 };
	const size_t length =
		ROUND_UP(UBI_HEADER_MAGIC_SIZE, ubi->geometry.write_block_size);
	int ret = 0;

	if (UBI_HEADER_OK == headers->ec_status) {
		ret = ubi_impl_io_write(ubi, pnum, UBI_EC_HEADER_OFFSET, zeroes,
					length);

		if (0 != ret)
			return ret;
	}

	if (UBI_HEADER_OK == headers->vid_status) {
		ret = ubi_impl_io_write(ubi, pnum, UBI_VID_HEADER_OFFSET,
					zeroes, length);

		if (0 != ret)
			return ret;
	}

	return 0;
}

static int peb_erase_failed(struct ubi_device *ubi, uint32_t pnum, int ret)
{
	/* Formatting has no device to stop. */
	if (NULL != ubi->blocks.state && !ubi->read_only) {
		LOG_ERR("PEB %u: could not be erased (%d), so the device is "
			"read-only until it is attached again",
			pnum, ret);
		ubi->read_only = true;
	}

	return ret;
}

static int peb_purge_next(const struct ubi_device *ubi, uint32_t vol_id,
			  uint32_t first, uint32_t count, uint32_t *pnum,
			  uint32_t *lnum)
{
	uint64_t chosen_sqnum = 0;
	bool chosen_unmapped = false;
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, candidate);
		const bool unmapped = (UBI_PEB_UNMAPPED == state);
		struct ubi_headers headers = { 0 };

		if (UBI_PEB_RECLAIM != state && !unmapped)
			continue;

		const int ret = ubi_impl_header_read(ubi, candidate, &headers);

		if (0 != ret)
			return ret;

		if (UBI_HEADER_OK != headers.vid_status ||
		    vol_id != headers.vid.vol_id || headers.vid.lnum < first ||
		    headers.vid.lnum - first >= count)
			continue;

		/* The one an unmap let go of may be the newest copy left. */
		const bool later = found &&
				   ((unmapped && !chosen_unmapped) ||
				    (unmapped == chosen_unmapped &&
				     headers.vid.sqnum >= chosen_sqnum));

		if (later)
			continue;

		chosen_sqnum = headers.vid.sqnum;
		chosen_unmapped = unmapped;
		found = true;
		*pnum = candidate;
		*lnum = headers.vid.lnum;
	}

	if (!found)
		return -ENOENT;

	return 0;
}

/* Module interface function definitions ----------------------------------- */

enum ubi_peb_state ubi_impl_peb_state_get(const struct ubi_device *ubi,
					  uint32_t pnum)
{
	return (enum ubi_peb_state)ubi->blocks.state[pnum];
}

void ubi_impl_peb_state_set(struct ubi_device *ubi, uint32_t pnum,
			    enum ubi_peb_state state)
{
	ubi->blocks.state[pnum] = (uint8_t)state;
}

int ubi_impl_peb_prepare(struct ubi_device *ubi, uint32_t pnum)
{
	struct ubi_headers headers = { 0 };
	uint64_t erase_count = 0;
	int ret = ubi_impl_header_read(ubi, pnum, &headers);

	if (0 != ret)
		return peb_erase_failed(ubi, pnum, ret);

	/* An attached device keeps every count in RAM, which is what makes
	 * clearing the header safe; formatting has only the header. */
	if (NULL != ubi->blocks.erase_count)
		erase_count = ubi->blocks.erase_count[pnum];
	else if (UBI_HEADER_OK == headers.ec_status)
		erase_count = headers.ec.erase_count;

	if (erase_count >= UBI_MAX_ERASE_COUNT) {
		LOG_ERR("PEB %u: erased %llu times, as many as an erase counter "
			"holds",
			pnum, erase_count);
		return peb_erase_failed(ubi, pnum, -EINVAL);
	}

	if (IS_ENABLED(CONFIG_UBI_ERASE_INVALIDATES_HEADERS) &&
	    !ubi->invalidation_refused) {
		ret = peb_invalidate(ubi, pnum, &headers);

		/* Only the protection against a torn erase is lost. */
		if (0 != ret) {
			LOG_WRN("PEB %u: the flash refuses to write over a "
				"header, so blocks are erased without clearing "
				"them; turn CONFIG_UBI_ERASE_INVALIDATES_HEADERS "
				"off for this flash",
				pnum);
			ubi->invalidation_refused = true;
		}
	}

	ret = ubi_impl_io_erase(ubi, pnum);

	if (0 != ret)
		return peb_erase_failed(ubi, pnum, ret);

	const struct ubi_ec_header header = {
		.erase_count = erase_count + 1,
		.image_seq = ubi->image_seq,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};

	/* The erase happened whether or not the stamp below lands. */
	if (NULL != ubi->blocks.erase_count)
		ubi->blocks.erase_count[pnum] = (uint32_t)header.erase_count;

	ret = ubi_impl_header_ec_write(ubi, pnum, &header);

	if (0 != ret)
		return ret;

	/* Formatting stamps blocks before there is anywhere to record their
	 * state. */
	if (NULL == ubi->blocks.state)
		return 0;

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_FREE);
	peb_protection_tick(ubi, pnum);

	return 0;
}

int ubi_impl_peb_reclaim(struct ubi_device *ubi, uint32_t pnum)
{
	const enum ubi_peb_state state = ubi_impl_peb_state_get(ubi, pnum);
	int ret = 0;

	if (UBI_PEB_UNMAPPED == state) {
		struct ubi_headers headers = { 0 };

		ret = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != ret) {
			ubi_impl_peb_retire(ubi, pnum, UBI_VOL_ID_INVALID, 0);
			return peb_erase_failed(ubi, pnum, ret);
		}

		/* Retires whatever refuses on its own. */
		if (UBI_HEADER_OK == headers.vid_status)
			return ubi_impl_peb_purge(ubi, headers.vid.vol_id,
						  headers.vid.lnum, 1);
	}

	ret = ubi_impl_peb_prepare(ubi, pnum);

	if (0 != ret)
		ubi_impl_peb_retire(ubi, pnum, UBI_VOL_ID_INVALID, 0);

	return ret;
}

int ubi_impl_peb_allocate_for_write(struct ubi_device *ubi, uint32_t *pnum)
{
	uint32_t candidate = 0;
	const int found = peb_free_most_worn_below(
		ubi, CONFIG_UBI_WEAR_LEVELING_THRESHOLD, &candidate);

	if (-ENOENT == found) {
		const int erased = peb_erase_for_write(ubi, &candidate);

		if (0 != erased)
			return erased;
	} else if (0 != found) {
		return found;
	}

	/* The least worn free block is the first thing levelling would reach
	 * for, and moving it now would carry data the caller has only begun
	 * writing. */
	ubi->blocks.protect[candidate] = (uint8_t)CONFIG_UBI_PROTECTION_CYCLES;
	*pnum = candidate;

	return 0;
}

int ubi_impl_peb_allocate_for_levelling(const struct ubi_device *ubi,
					uint32_t *pnum)
{
	const int ret = peb_free_most_worn_below(
		ubi, 2 * CONFIG_UBI_WEAR_LEVELING_THRESHOLD, pnum);

	/* Quietly: device info asks this every time it is read. */
	if (0 != ret)
		return -ENOSPC;

	return 0;
}

void ubi_impl_peb_retire(struct ubi_device *ubi, uint32_t pnum, uint32_t vol_id,
			 uint32_t lnum)
{
	LOG_WRN("PEB %u: retired after a failed write or erase", pnum);

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
	ubi_impl_event_emit(ubi, UBI_EVENT_PEB_BAD, pnum, vol_id, lnum);
}

void ubi_impl_peb_withdraw(struct ubi_device *ubi, uint32_t pnum,
			   uint32_t vol_id, uint32_t lnum)
{
	const int ret = ubi_impl_peb_prepare(ubi, pnum);

	if (0 != ret)
		LOG_ERR("PEB %u: a write to volume %u block %u failed and could "
			"not be erased (%d); the next attach may take it",
			pnum, vol_id, lnum, ret);

	ubi_impl_peb_retire(ubi, pnum, vol_id, lnum);
}

void ubi_impl_peb_write_off(struct ubi_device *ubi, uint32_t pnum)
{
	LOG_WRN("PEB %u: failed again and is out of service for good", pnum);

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_WORN_OUT);
}

int ubi_impl_peb_purge(struct ubi_device *ubi, uint32_t vol_id, uint32_t first,
		       uint32_t count)
{
	for (;;) {
		uint32_t pnum = 0;
		uint32_t lnum = 0;
		int ret =
			peb_purge_next(ubi, vol_id, first, count, &pnum, &lnum);

		if (-ENOENT == ret)
			return 0;

		if (0 != ret)
			return ret;

		ret = ubi_impl_peb_prepare(ubi, pnum);

		if (0 != ret) {
			ubi_impl_peb_retire(ubi, pnum, vol_id, lnum);
			return ret;
		}
	}
}
