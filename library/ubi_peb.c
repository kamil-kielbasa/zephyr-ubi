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
 * \brief Average erase count of the blocks that still have a readable one.
 */
static uint32_t peb_mean_erase_count(const struct ubi_device *ubi);

/* Static function definitions --------------------------------------------- */

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

	return found ? 0 : -ENOENT;
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

		if (UBI_PEB_UNKNOWN == state || UBI_PEB_BAD == state)
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

int ubi_peb_prepare(struct ubi_device *ubi, uint32_t pnum, bool *history_lost)
{
	struct ubi_headers headers = { 0 };
	int ret = ubi_headers_read(ubi, pnum, &headers);

	if (0 != ret)
		return ret;

	const bool readable = (UBI_HEADER_OK == headers.ec_status);
	const bool blank = (UBI_HEADER_ERASED == headers.ec_status);
	uint64_t erase_count = 0;

	if (readable)
		erase_count = headers.ec.erase_count;
	else if (!blank)
		erase_count = peb_mean_erase_count(ubi);

	*history_lost = !readable && !blank;

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

	return 0;
}

int ubi_peb_allocate(struct ubi_device *ubi, uint32_t *pnum)
{
	uint32_t candidate = 0;
	int ret = peb_least_worn(ubi, UBI_PEB_FREE, &candidate);

	if (0 == ret) {
		*pnum = candidate;
		return 0;
	}

	ret = peb_least_worn(ubi, UBI_PEB_UNKNOWN, &candidate);

	if (0 != ret) {
		LOG_ERR("no block is free and none can be made free without "
			"reclaiming");
		return -ENOSPC;
	}

	bool history_lost = false;

	ret = ubi_peb_prepare(ubi, candidate, &history_lost);

	if (0 != ret) {
		LOG_ERR("PEB %u cannot be made ready for use (%d)", candidate,
			ret);
		ubi_peb_state_set(ubi, candidate, UBI_PEB_BAD);
		return ret;
	}

	if (history_lost)
		LOG_WRN("PEB %u: its erase count was unreadable, so it starts "
			"again from the device average",
			candidate);

	*pnum = candidate;

	return 0;
}
