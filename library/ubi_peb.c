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

/* UBI headers: */
#include "ubi_event.h"
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_peb.h"
#include "ubi_private.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Find the least worn block in a given state.
 */
static int peb_least_worn(const struct ubi_device *ubi,
			  enum ubi_peb_state state, uint32_t *pnum);

/**
 * \brief Find the most worn block in a given state.
 */
static int peb_most_worn(const struct ubi_device *ubi, enum ubi_peb_state state,
			 uint32_t *pnum);

/**
 * \brief Average erase count of the blocks that still have a readable one.
 */
static uint32_t peb_mean_erase_count(const struct ubi_device *ubi);

/**
 * \brief Bring every protection countdown one erase closer to expiring.
 *
 * \param erased                        Block that was just erased, which
 *                                      now holds nothing worth protecting.
 */
static void peb_protection_tick(struct ubi_device *ubi, uint32_t erased);

/**
 * \brief Say the other way round that no block in \p state beats \p chosen.
 *
 *        A search that has drifted still agrees with itself, so this states
 *        the property instead of looking for it again.
 *
 * \retval 0
 *         The choice holds.
 * \retval -EFAULT
 *         It does not, and the library has contradicted itself.
 */
static int peb_check_extreme(const struct ubi_device *ubi,
			     enum ubi_peb_state state, uint32_t chosen,
			     bool lowest);

/* Static function definitions --------------------------------------------- */

static void peb_protection_tick(struct ubi_device *ubi, uint32_t erased)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		if (0 != ubi->blocks.protect[pnum])
			ubi->blocks.protect[pnum] -= 1;
	}

	ubi->blocks.protect[erased] = 0;
}

static int peb_check_extreme(const struct ubi_device *ubi,
			     enum ubi_peb_state state, uint32_t chosen,
			     bool lowest)
{
	const uint32_t taken = ubi->blocks.erase_count[chosen];

	if (state != ubi_peb_state_get(ubi, chosen)) {
		LOG_ERR("PEB %u was chosen out of the wrong state", chosen);
		return -EFAULT;
	}

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const uint32_t count = ubi->blocks.erase_count[pnum];

		if (state != ubi_peb_state_get(ubi, pnum))
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

static int peb_least_worn(const struct ubi_device *ubi,
			  enum ubi_peb_state state, uint32_t *pnum)
{
	uint32_t lowest = UINT32_MAX;
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		if (state != ubi_peb_state_get(ubi, candidate))
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

static int peb_most_worn(const struct ubi_device *ubi, enum ubi_peb_state state,
			 uint32_t *pnum)
{
	uint32_t highest = 0;
	bool found = false;

	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		if (state != ubi_peb_state_get(ubi, candidate))
			continue;

		if (found && ubi->blocks.erase_count[candidate] <= highest)
			continue;

		highest = ubi->blocks.erase_count[candidate];
		*pnum = candidate;
		found = true;
	}

	if (!found)
		return -ENOENT;

	if (IS_ENABLED(CONFIG_UBI_SELF_CHECKS))
		return peb_check_extreme(ubi, state, *pnum, false);

	return 0;
}

static uint32_t peb_mean_erase_count(const struct ubi_device *ubi)
{
	uint64_t total = 0;
	uint32_t counted = 0;

	/* Formatting stamps blocks before there is a table to average over. */
	if (NULL == ubi->blocks.erase_count)
		return 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state = ubi_peb_state_get(ubi, pnum);

		if (UBI_PEB_UNKNOWN == state || UBI_PEB_BAD == state ||
		    UBI_PEB_WORN_OUT == state)
			continue;

		total += ubi->blocks.erase_count[pnum];
		counted += 1;
	}

	if (0 == counted)
		return 0;

	return (uint32_t)(total / counted);
}

/* Module interface function definitions ----------------------------------- */

enum ubi_peb_state ubi_peb_state_get(const struct ubi_device *ubi,
				     uint32_t pnum)
{
	return (enum ubi_peb_state)ubi->blocks.state[pnum];
}

void ubi_peb_state_set(struct ubi_device *ubi, uint32_t pnum,
		       enum ubi_peb_state state)
{
	ubi->blocks.state[pnum] = (uint8_t)state;
}

int ubi_peb_prepare(struct ubi_device *ubi, uint32_t pnum)
{
	struct ubi_headers headers = { 0 };
	int ret = ubi_headers_read(ubi, pnum, &headers);

	if (0 != ret)
		return ret;

	const bool readable = (UBI_HEADER_OK == headers.ec_status);
	const bool blank = (UBI_HEADER_ERASED == headers.ec_status);
	uint64_t erase_count = 0;

	if (readable) {
		erase_count = headers.ec.erase_count;
	} else if (!blank) {
		erase_count = peb_mean_erase_count(ubi);
		LOG_WRN("PEB %u: its erase count was unreadable, so it starts "
			"again from the device average",
			pnum);
	}

	ret = ubi_io_erase(ubi, pnum);

	if (0 != ret)
		return ret;

	const struct ubi_ec_header header = {
		.erase_count = erase_count + 1,
		.image_seq = ubi->image_seq,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};

	ret = ubi_ec_header_write(ubi, pnum, &header);

	if (0 != ret)
		return ret;

	/* Formatting has no block tables; it stamps blocks before there is
	 * anywhere to record their state. */
	if (NULL == ubi->blocks.state)
		return 0;

	ubi->blocks.erase_count[pnum] = (uint32_t)header.erase_count;
	ubi_peb_state_set(ubi, pnum, UBI_PEB_FREE);
	peb_protection_tick(ubi, pnum);

	return 0;
}

int ubi_peb_allocate(struct ubi_device *ubi, uint32_t *pnum)
{
	uint32_t candidate = 0;
	int ret = peb_least_worn(ubi, UBI_PEB_FREE, &candidate);

	if (0 != ret) {
		ret = peb_least_worn(ubi, UBI_PEB_UNKNOWN, &candidate);

		if (0 != ret) {
			LOG_ERR("no block is free and none can be made free "
				"without reclaiming");
			return -ENOSPC;
		}

		ret = ubi_peb_prepare(ubi, candidate);

		if (0 != ret) {
			ubi_peb_retire(ubi, candidate, UBI_VOL_ID_INVALID, 0);
			return ret;
		}
	}

	/*
	 * This block was handed over because it was the least worn one free,
	 * which makes it the least worn one in use the moment anything lands
	 * on it, and so the first thing levelling would reach for. Moving it
	 * now would carry data the caller has not finished writing.
	 */
	ubi->blocks.protect[candidate] = (uint8_t)CONFIG_UBI_PROTECTION_CYCLES;
	*pnum = candidate;

	return 0;
}

int ubi_peb_allocate_worn(struct ubi_device *ubi, uint32_t *pnum)
{
	const int ret = peb_most_worn(ubi, UBI_PEB_FREE, pnum);

	if (0 != ret) {
		LOG_ERR("no block is erased and waiting to be written");
		return -ENOSPC;
	}

	return 0;
}

void ubi_peb_retire(struct ubi_device *ubi, uint32_t pnum, uint32_t vol_id,
		    uint32_t lnum)
{
	LOG_WRN("PEB %u: retired after a failed write or erase", pnum);

	ubi_peb_state_set(ubi, pnum, UBI_PEB_BAD);
	ubi_event_emit(ubi, UBI_EVENT_PEB_BAD, pnum, vol_id, lnum);
}

void ubi_peb_write_off(struct ubi_device *ubi, uint32_t pnum)
{
	LOG_WRN("PEB %u: failed again and is out of service for good", pnum);

	ubi_peb_state_set(ubi, pnum, UBI_PEB_WORN_OUT);
}
