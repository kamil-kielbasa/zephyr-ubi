/**
 * \file    ubi_attach.c
 * \author  Kamil Kielbasa
 * \brief   Rebuilding a device in RAM from what its partition holds.
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
#include <zephyr/storage/flash_map.h>

/* UBI headers: */
#include "ubi_attach.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_scan.h"
#include "ubi_state.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Sentinel for "no copy of the volume table was adopted". */
#define VOLUME_TABLE_COPY_NONE (UINT32_MAX)

/** Attach refuses a partition with a twentieth of its good blocks corrupt,
 *  or eight when that share rounds down to none, as Linux UBI does. */
#define CORRUPT_PEB_SHARE (20)
#define CORRUPT_PEB_SMALL (8)

BUILD_ASSERT(UBI_VOLUME_TABLE_LEB_COUNT == 2,
	     "attach picks the newer volume table copy assuming there are two");

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief What one copy of the volume table record turned out to hold.
 */
struct volume_table_copy {
	/** The copy verified and decoded. */
	bool readable;
	/** Image it belongs to, meaningful only when \ref readable. */
	uint32_t image_seq;
	/** Revision it carries, meaningful only when \ref readable. */
	uint32_t revision;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Give every block whose erase counter could not be read the mean of
 *        those that could; zero would make it the first block every
 *        allocation reaches for.
 */
static void attach_erase_counts_settle(struct ubi_device *ubi);

/**
 * \brief Keep only the copies that carry the record adopted, and report the
 *        table degraded unless both do.
 */
static void attach_table_keep_current(struct ubi_device *ubi,
				      const struct volume_table_copy *copy,
				      uint32_t adopted);

/**
 * \brief Say why no copy of the volume table could be adopted.
 *
 * \param found                         Copies with a block behind them.
 * \param failure                       What reading them came to.
 *
 * \return \c -ENODEV when nothing says the partition was ever a UBI device,
 *         and the reason it cannot be attached otherwise.
 */
static int attach_table_missing(const struct ubi_device *ubi,
				const struct ubi_scan *scan, uint32_t found,
				int failure);

/**
 * \brief Read both copies, the older first, so that the record left in the
 *        scratch buffer is the newest one that reads back.
 *
 * \retval 0
 *         A copy was adopted.
 * \retval -ENODEV
 *         Nothing on the partition says it was ever a UBI device.
 * \retval -EBADMSG
 *         UBI metadata is there but none of it verifies.
 * \retval -ENOTSUP
 *         A copy that may be the table in force was written by a release
 *         this build cannot read.
 * \retval -EIO
 *         A copy that may be the table in force could not be read.
 */
static int attach_table_adopt(struct ubi_device *ubi,
			      const struct ubi_scan *scan);

/**
 * \brief Refuse a device this build cannot read, or one formatted for
 *        another geometry.
 */
static int attach_record_check(const struct ubi_device *ubi,
			       const struct ubi_scan *scan);

/**
 * \brief Drop a copy whose block the second pass took away from this image,
 *        so that the next update does not erase it after it was handed out.
 */
static void attach_table_settle(struct ubi_device *ubi);

/**
 * \brief Refuse a partition that carries more damage than it can be worth
 *        attaching.
 *
 * \retval 0
 *         Few enough blocks are corrupt to carry on.
 * \retval -EINVAL
 *         Too many.
 */
static int attach_corruption_check(const struct ubi_device *ubi);

/* Static function definitions --------------------------------------------- */

static void attach_erase_counts_settle(struct ubi_device *ubi)
{
	uint64_t total = 0;
	uint32_t counted = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		if (UBI_ERASE_COUNT_UNKNOWN == ubi->blocks.erase_count[pnum])
			continue;

		total += ubi->blocks.erase_count[pnum];
		counted += 1;
	}

	const uint32_t mean = (0 == counted) ? 0 : (uint32_t)(total / counted);

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		if (UBI_ERASE_COUNT_UNKNOWN == ubi->blocks.erase_count[pnum])
			ubi->blocks.erase_count[pnum] = mean;
	}
}

static void attach_table_keep_current(struct ubi_device *ubi,
				      const struct volume_table_copy *copy,
				      uint32_t adopted)
{
	const struct ubi_volume_table_record *record = &ubi->scratch->record;
	bool agree = true;

	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum) {
		const bool current = copy[lnum].readable &&
				     copy[lnum].image_seq ==
					     record->image_seq &&
				     copy[lnum].revision == record->revision;

		if (current)
			continue;

		/* Relocation would seal it afresh, above the current copy; the
		 * second pass queues its block for reclaim. */
		agree = false;
		ubi->volume_table.eba[lnum] = UBI_LEB_UNMAPPED;
	}

	if (agree)
		return;

	LOG_WRN("partition %u: not every copy of the volume table is "
		"current, so one erase would cost a revision",
		ubi->flash_area->fa_id);
	ubi->volume_table.degraded = true;
	ubi_impl_event_emit(ubi, UBI_EVENT_VOLUME_TABLE_DEGRADED,
			    ubi->volume_table.eba[adopted],
			    UBI_VOLUME_TABLE_VOL_ID, adopted);
}

static int attach_table_missing(const struct ubi_device *ubi,
				const struct ubi_scan *scan, uint32_t found,
				int failure)
{
	const uint8_t id = ubi->flash_area->fa_id;

	if (0 != found) {
		LOG_ERR("partition %u: none of its %u volume table copies "
			"could be used",
			id, found);
		return failure;
	}

	if (0 != scan->unopenable) {
		LOG_ERR("partition %u: %u headers do not verify; the key is "
			"wrong or the metadata was modified",
			id, scan->unopenable);
		return -EBADMSG;
	}

	if (0 != scan->unsupported) {
		LOG_ERR("partition %u: %u headers were written by a release "
			"this build cannot read",
			id, scan->unsupported);
		return -ENOTSUP;
	}

	if (0 != scan->claimed) {
		LOG_ERR("partition %u: %u blocks hold data but no volume table "
			"is left to describe them",
			id, scan->claimed);
		return -EBADMSG;
	}

	LOG_INF("partition %u holds no UBI device", id);

	return -ENODEV;
}

static int attach_table_adopt(struct ubi_device *ubi,
			      const struct ubi_scan *scan)
{
	const uint32_t newest =
		(ubi->volume_table.sqnum[0] >= ubi->volume_table.sqnum[1]) ? 0 :
									     1;
	const uint32_t order[UBI_VOLUME_TABLE_LEB_COUNT] = { 1 - newest,
							     newest };
	struct ubi_volume_table_record *record = &ubi->scratch->record;
	struct volume_table_copy copy[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t adopted = VOLUME_TABLE_COPY_NONE;
	uint32_t found = 0;
	int failure = -EBADMSG;

	for (uint32_t i = 0; i < UBI_VOLUME_TABLE_LEB_COUNT; ++i) {
		const uint32_t lnum = order[i];
		const uint32_t pnum = ubi->volume_table.eba[lnum];

		if (UBI_LEB_UNMAPPED == pnum)
			continue;

		found += 1;

		const int ret = ubi_impl_volume_table_read(ubi, lnum, record);
		const bool unreadable = (-EIO == ret || -ENOTSUP == ret);

		/* The newer copy may be the table in force, so one that cannot
		 * be read, now or by this build, stops the attach rather than
		 * let the older one stand in for it. */
		if (newest == lnum && unreadable) {
			LOG_ERR("PEB %u holds the newer volume table copy %u "
				"and it cannot be read (%d)",
				pnum, lnum, ret);
			return ret;
		}

		if (0 != ret) {
			ubi_impl_event_emit(ubi, UBI_EVENT_VOLUME_TABLE_CORRUPT,
					    pnum, UBI_VOLUME_TABLE_VOL_ID,
					    lnum);
			LOG_ERR("PEB %u holds volume table copy %u and it is "
				"unusable (%d)",
				pnum, lnum, ret);

			/* A copy that could not be read, or that a newer
			 * release wrote, is not a damaged one; saying so is
			 * what stops an application from formatting. */
			if (-EIO == ret || (-ENOTSUP == ret && -EIO != failure))
				failure = ret;

			continue;
		}

		copy[lnum].readable = true;
		copy[lnum].image_seq = record->image_seq;
		copy[lnum].revision = record->revision;
		adopted = lnum;
	}

	if (VOLUME_TABLE_COPY_NONE == adopted)
		return attach_table_missing(ubi, scan, found, failure);

	attach_table_keep_current(ubi, copy, adopted);
	ubi->volume_table.current = adopted;

	return 0;
}

static int attach_record_check(const struct ubi_device *ubi,
			       const struct ubi_scan *scan)
{
	const struct ubi_volume_table_record *record = &ubi->scratch->record;
	const uint8_t id = ubi->flash_area->fa_id;

	/* A block a newer release wrote would otherwise be taken for blank
	 * and erased. */
	if (0 != scan->unsupported) {
		LOG_ERR("partition %u: %u headers were written by a release "
			"this build cannot read",
			id, scan->unsupported);
		return -ENOTSUP;
	}

	if (record->peb_size != ubi->geometry.peb_size ||
	    record->peb_count != ubi->geometry.peb_count) {
		LOG_ERR("partition %u is %u blocks of %u bytes, the volume "
			"table was written for %u of %u",
			id, ubi->geometry.peb_count, ubi->geometry.peb_size,
			record->peb_count, record->peb_size);
		return -EINVAL;
	}

	return 0;
}

static void attach_table_settle(struct ubi_device *ubi)
{
	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum) {
		const uint16_t pnum = ubi->volume_table.eba[lnum];

		if (UBI_LEB_UNMAPPED == pnum)
			continue;

		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_MAPPED == state)
			continue;

		LOG_WRN("PEB %u: holds volume table copy %u of another image",
			pnum, lnum);
		ubi->volume_table.eba[lnum] = UBI_LEB_UNMAPPED;
	}
}

static int attach_corruption_check(const struct ubi_device *ubi)
{
	uint32_t bad = 0;
	uint32_t corrupted = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_BAD == state)
			bad += 1;

		if (UBI_PEB_CORRUPT == state)
			corrupted += 1;
	}

	const uint32_t share =
		(ubi->geometry.peb_count - bad) / CORRUPT_PEB_SHARE;
	const uint32_t allowed = (0 != share) ? share : CORRUPT_PEB_SMALL;

	if (corrupted < allowed)
		return 0;

	LOG_ERR("partition %u: %u of %u blocks are corrupt, attach needs "
		"fewer than %u",
		ubi->flash_area->fa_id, corrupted, ubi->geometry.peb_count,
		allowed);

	return -EINVAL;
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_attach(struct ubi_device *ubi)
{
	const struct ubi_volume_table_record *record = &ubi->scratch->record;
	struct ubi_scan scan = { 0 };
	int ret = ubi_impl_scan_first_pass(ubi, &scan);

	if (0 != ret)
		return ret;

	attach_erase_counts_settle(ubi);

	ret = attach_table_adopt(ubi, &scan);

	if (0 != ret)
		return ret;

	ret = attach_record_check(ubi, &scan);

	if (0 != ret)
		return ret;

	ubi->image_seq = record->image_seq;

	ret = ubi_impl_volumes_build(ubi, record);

	if (0 != ret) {
		LOG_ERR("the volume table declares more logical blocks than "
			"the %u this partition has",
			ubi->geometry.peb_count);
		return ret;
	}

	ret = ubi_impl_scan_second_pass(ubi);

	if (0 != ret)
		return ret;

	attach_table_settle(ubi);

	return attach_corruption_check(ubi);
}
