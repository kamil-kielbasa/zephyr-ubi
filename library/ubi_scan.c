/**
 * \file    ubi_scan.c
 * \author  Kamil Kielbasa
 * \brief   The two passes attach makes over the partition.
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
#include <zephyr/sys/util.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_scan.h"
#include "ubi_state.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Classify a block whose erase counter header verified but whose
 *        volume identifier header did not: a blank data area means a write
 *        cut short, anything else is damage kept for what it holds.
 *
 * \param[out] state                    #UBI_PEB_UNKNOWN or #UBI_PEB_CORRUPT.
 */
static int scan_damaged_state(const struct ubi_device *ubi, uint32_t pnum,
			      enum ubi_peb_state *state);

/**
 * \brief Report whether a block's data matches the seal of its header.
 *
 * \retval 0
 *         \p holds says whether it does.
 * \retval -EIO
 *         The flash driver failed.
 */
static int scan_seal_holds(const struct ubi_device *ubi, uint32_t pnum,
			   const struct ubi_vid_header *vid, bool *holds);

/**
 * \brief Act on what the erase counter header turned out to be.
 *
 * \return Whether the block is worth looking at any further.
 */
static bool scan_ec(struct ubi_device *ubi, uint32_t pnum,
		    const struct ubi_headers *headers, struct ubi_scan *scan);

/**
 * \brief Act on what the volume identifier header turned out to be.
 *
 * \return 1 when the block backs a logical erase block, 0 when it has been
 *         classified otherwise, or a negative error code.
 */
static int scan_vid(struct ubi_device *ubi, uint32_t pnum,
		    const struct ubi_headers *headers, struct ubi_scan *scan);

/**
 * \brief Settle two blocks claiming one volume table copy: the newer wins
 *        only if its data matches its seal.
 *
 * \param[out] wins                     Whether \p pnum takes the copy from
 *                                      \p incumbent.
 */
static int scan_table_copy_wins(const struct ubi_device *ubi, uint32_t pnum,
				const struct ubi_vid_header *vid,
				uint16_t incumbent, bool *wins);

/**
 * \brief Remember which block holds each copy of the volume table.
 */
static int scan_volume_table(struct ubi_device *ubi, uint32_t pnum,
			     const struct ubi_vid_header *vid);

/**
 * \brief Classify one block by the headers it carries. A block that cannot
 *        be read stops the attach: it says nothing about what it holds.
 */
static int scan_peb(struct ubi_device *ubi, uint32_t pnum,
		    struct ubi_scan *scan);

/**
 * \brief Queue for reclaim a block claiming a volume table copy the first
 *        pass did not settle on.
 */
static void scan_claim_table(struct ubi_device *ubi, uint32_t pnum,
			     const struct ubi_vid_header *vid);

/**
 * \brief Queue for reclaim a block naming a logical block the volume table
 *        does not describe, and report it unless a removal or a shrink of
 *        this device left it.
 */
static void scan_claim_orphan(struct ubi_device *ubi, uint32_t pnum,
			      const struct ubi_vid_header *vid);

/**
 * \brief Hang a block off the logical block it names, unless a newer copy
 *        has it.
 *
 *        Of two copies the newer wins only if its data matches its seal, so
 *        a change cut short falls back to the copy before it. A lone copy is
 *        kept as it reads; either way a failed seal is reported.
 */
static int scan_claim(struct ubi_device *ubi, uint32_t pnum,
		      const struct ubi_vid_header *vid);

/* Static function definitions --------------------------------------------- */

static int scan_damaged_state(const struct ubi_device *ubi, uint32_t pnum,
			      enum ubi_peb_state *state)
{
	uint8_t chunk[CONFIG_UBI_IO_CHUNK_SIZE];

	for (uint32_t at = 0; at < ubi->geometry.leb_size;
	     at += sizeof(chunk)) {
		const uint32_t length =
			MIN(sizeof(chunk), ubi->geometry.leb_size - at);
		const int ret =
			ubi_impl_io_read_data(ubi, pnum, at, chunk, length);

		if (0 != ret)
			return ret;

		for (uint32_t i = 0; i < length; ++i) {
			if (ubi->geometry.erase_value != chunk[i]) {
				*state = UBI_PEB_CORRUPT;
				return 0;
			}
		}
	}

	*state = UBI_PEB_UNKNOWN;

	return 0;
}

static int scan_seal_holds(const struct ubi_device *ubi, uint32_t pnum,
			   const struct ubi_vid_header *vid, bool *holds)
{
	const int ret = ubi_impl_header_vid_data_verify(ubi, pnum, vid);

	if (0 != ret && -EBADMSG != ret)
		return ret;

	*holds = (0 == ret);

	return 0;
}

static bool scan_ec(struct ubi_device *ubi, uint32_t pnum,
		    const struct ubi_headers *headers, struct ubi_scan *scan)
{
	switch (headers->ec_status) {
	case UBI_HEADER_OK:
		break;
	case UBI_HEADER_ERASED:
	case UBI_HEADER_NOT_UBI:
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	case UBI_HEADER_CORRUPT:
		/* An interrupted stamp; the block itself is fine. */
		ubi_impl_event_emit(ubi, UBI_EVENT_HDR_CORRUPT, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	case UBI_HEADER_TAMPERED:
		/* Counted, so that a wrong key does not pass for a blank
		 * partition, and erased before use like any leftover. */
		scan->unopenable += 1;
		ubi_impl_event_emit(ubi, UBI_EVENT_HDR_TAMPERED, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	case UBI_HEADER_UNSUPPORTED:
		scan->unsupported += 1;
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	case UBI_HEADER_ERROR:
	default:
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	}

	if (headers->ec.erase_count > UBI_MAX_ERASE_COUNT) {
		LOG_ERR("PEB %u: erase count %llu is past what UBI writes, "
			"taking the block out of service",
			pnum, headers->ec.erase_count);
		ubi_impl_event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
		return false;
	}

	return true;
}

static int scan_vid(struct ubi_device *ubi, uint32_t pnum,
		    const struct ubi_headers *headers, struct ubi_scan *scan)
{
	enum ubi_peb_state state = UBI_PEB_UNKNOWN;

	/* The erase counter header verified under this key, so damage behind
	 * it condemns one block, not the whole device. */
	switch (headers->vid_status) {
	case UBI_HEADER_OK:
		return 1;
	case UBI_HEADER_ERASED:
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_FREE);
		return 0;
	case UBI_HEADER_NOT_UBI:
		break;
	case UBI_HEADER_CORRUPT:
		ubi_impl_event_emit(ubi, UBI_EVENT_HDR_CORRUPT, pnum,
				    UBI_VOL_ID_INVALID, 0);
		break;
	case UBI_HEADER_TAMPERED:
		ubi_impl_event_emit(ubi, UBI_EVENT_HDR_TAMPERED, pnum,
				    UBI_VOL_ID_INVALID, 0);
		break;
	case UBI_HEADER_UNSUPPORTED:
		scan->unsupported += 1;
		break;
	case UBI_HEADER_ERROR:
	default:
		return -EIO;
	}

	const int ret = scan_damaged_state(ubi, pnum, &state);

	if (0 != ret)
		return ret;

	ubi_impl_peb_state_set(ubi, pnum, state);

	return 0;
}

static int scan_table_copy_wins(const struct ubi_device *ubi, uint32_t pnum,
				const struct ubi_vid_header *vid,
				uint16_t incumbent, bool *wins)
{
	struct ubi_headers held = { 0 };
	bool holds = false;
	int ret = 0;

	if (vid->sqnum > ubi->volume_table.sqnum[vid->lnum]) {
		ret = scan_seal_holds(ubi, pnum, vid, &holds);

		if (0 != ret)
			return ret;

		*wins = holds;

		return 0;
	}

	ret = ubi_impl_header_read(ubi, incumbent, &held);

	if (0 != ret)
		return ret;

	if (UBI_HEADER_OK != held.vid_status)
		return -EIO;

	ret = scan_seal_holds(ubi, incumbent, &held.vid, &holds);

	if (0 != ret)
		return ret;

	*wins = !holds;

	return 0;
}

static int scan_volume_table(struct ubi_device *ubi, uint32_t pnum,
			     const struct ubi_vid_header *vid)
{
	/* Left mapped for the second pass to queue for reclaim. */
	if (vid->lnum >= UBI_VOLUME_TABLE_LEB_COUNT) {
		LOG_WRN("PEB %u: claims volume table copy %u, of which there "
			"are only %d",
			pnum, vid->lnum, UBI_VOLUME_TABLE_LEB_COUNT);
		return 0;
	}

	const uint16_t incumbent = ubi->volume_table.eba[vid->lnum];
	bool wins = true;

	/* Formats and relocation can leave several blocks claiming a copy;
	 * the losers are left for the second pass. */
	if (UBI_LEB_UNMAPPED != incumbent) {
		const int ret =
			scan_table_copy_wins(ubi, pnum, vid, incumbent, &wins);

		if (0 != ret)
			return ret;
	}

	if (!wins) {
		LOG_DBG("PEB %u: holds a superseded volume table copy %u", pnum,
			vid->lnum);
		return 0;
	}

	ubi->volume_table.eba[vid->lnum] = (uint16_t)pnum;
	ubi->volume_table.sqnum[vid->lnum] = vid->sqnum;

	return 0;
}

static int scan_peb(struct ubi_device *ubi, uint32_t pnum,
		    struct ubi_scan *scan)
{
	struct ubi_headers headers = { 0 };
	int ret = ubi_impl_header_read(ubi, pnum, &headers);

	if (0 != ret) {
		LOG_ERR("PEB %u: unreadable (%d), so nothing can be said about "
			"what it holds",
			pnum, ret);
		return ret;
	}

	if (UBI_HEADER_ERROR == headers.ec_status ||
	    UBI_HEADER_ERROR == headers.vid_status) {
		LOG_ERR("PEB %u: the crypto backend could not check it", pnum);
		return -EIO;
	}

	const bool stamped = scan_ec(ubi, pnum, &headers, scan);

	if (!stamped)
		return 0;

	ubi->blocks.erase_count[pnum] = (uint32_t)headers.ec.erase_count;

	ret = scan_vid(ubi, pnum, &headers, scan);

	if (ret <= 0)
		return ret;

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

	if (headers.vid.sqnum > ubi->max_sqnum)
		ubi->max_sqnum = headers.vid.sqnum;

	if (UBI_VOLUME_TABLE_VOL_ID == headers.vid.vol_id)
		return scan_volume_table(ubi, pnum, &headers.vid);

	scan->claimed += 1;

	return 0;
}

static void scan_claim_table(struct ubi_device *ubi, uint32_t pnum,
			     const struct ubi_vid_header *vid)
{
	if (vid->lnum < UBI_VOLUME_TABLE_LEB_COUNT &&
	    pnum == ubi->volume_table.eba[vid->lnum])
		return;

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
}

static void scan_claim_orphan(struct ubi_device *ubi, uint32_t pnum,
			      const struct ubi_vid_header *vid)
{
	/* Identifiers are never reused, so one this device handed out is a
	 * volume removed or shrunk while its blocks were only queued. */
	if (vid->vol_id < ubi->volumes.id_watermark) {
		LOG_DBG("PEB %u: left by volume %u block %u", pnum, vid->vol_id,
			vid->lnum);
	} else {
		LOG_WRN("PEB %u: claims volume %u block %u, which this device "
			"never created; queued for reclaim",
			pnum, vid->vol_id, vid->lnum);
		ubi_impl_event_emit(ubi, UBI_EVENT_LEB_ORPHANED, pnum,
				    vid->vol_id, vid->lnum);
	}

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
}

static int scan_claim(struct ubi_device *ubi, uint32_t pnum,
		      const struct ubi_vid_header *vid)
{
	/* An erase counter header is stamped after an erase, so this image
	 * never wrote the identifier header behind one of its own. */
	if (vid->image_seq != ubi->image_seq) {
		LOG_WRN("PEB %u: stamped for this image but claimed for image "
			"0x%08x; queued for reclaim",
			pnum, vid->image_seq);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return 0;
	}

	if (UBI_VOLUME_TABLE_VOL_ID == vid->vol_id) {
		scan_claim_table(ubi, pnum, vid);
		return 0;
	}

	struct ubi_volume *volume = ubi_impl_volume_by_id(ubi, vid->vol_id);

	if (NULL == volume || vid->lnum >= volume->leb_count) {
		scan_claim_orphan(ubi, pnum, vid);
		return 0;
	}

	uint16_t incumbent = volume->eba[vid->lnum];
	bool holds = false;
	int ret = 0;

	if (UBI_LEB_UNMAPPED != incumbent) {
		struct ubi_headers held = { 0 };

		ret = ubi_impl_header_read(ubi, incumbent, &held);

		if (0 != ret)
			return ret;

		/* A block that will not say how old it is loses to one that
		 * will. */
		const bool newer = (UBI_HEADER_OK == held.vid_status &&
				    held.vid.sqnum > vid->sqnum);

		if (newer) {
			ret = scan_seal_holds(ubi, incumbent, &held.vid,
					      &holds);

			if (0 != ret)
				return ret;

			if (holds) {
				LOG_DBG("PEB %u: holds an older copy of volume "
					"%u block %u than PEB %u",
					pnum, vid->vol_id, vid->lnum,
					incumbent);
				ubi_impl_peb_state_set(ubi, pnum,
						       UBI_PEB_RECLAIM);
				return 0;
			}

			/* Reported when it was taken. */
			ubi_impl_peb_state_set(ubi, incumbent, UBI_PEB_RECLAIM);
			incumbent = UBI_LEB_UNMAPPED;
		}
	}

	ret = scan_seal_holds(ubi, pnum, vid, &holds);

	if (0 != ret)
		return ret;

	if (!holds) {
		ubi_impl_event_emit(ubi, UBI_EVENT_DATA_CORRUPT, pnum,
				    vid->vol_id, vid->lnum);

		if (UBI_LEB_UNMAPPED != incumbent) {
			LOG_WRN("PEB %u: volume %u block %u does not match the "
				"checksum it was sealed with; the older copy "
				"in PEB %u stands",
				pnum, vid->vol_id, vid->lnum, incumbent);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
			return 0;
		}

		LOG_WRN("PEB %u: volume %u block %u does not match the checksum "
			"it was sealed with and has no older copy; it is kept "
			"as it reads",
			pnum, vid->vol_id, vid->lnum);
	}

	if (UBI_LEB_UNMAPPED != incumbent) {
		LOG_DBG("PEB %u: holds an older copy of volume %u block %u "
			"than PEB %u",
			incumbent, vid->vol_id, vid->lnum, pnum);
		ubi_impl_peb_state_set(ubi, incumbent, UBI_PEB_RECLAIM);
	}

	volume->eba[vid->lnum] = (uint16_t)pnum;

	return 0;
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_scan_first_pass(struct ubi_device *ubi, struct ubi_scan *scan)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const int ret = scan_peb(ubi, pnum, scan);

		if (0 != ret)
			return ret;
	}

	return 0;
}

int ubi_impl_scan_second_pass(struct ubi_device *ubi)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);
		struct ubi_headers headers = { 0 };

		if (UBI_PEB_FREE != state && UBI_PEB_MAPPED != state &&
		    UBI_PEB_CORRUPT != state)
			continue;

		int ret = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != ret) {
			LOG_ERR("PEB %u: became unreadable (%d)", pnum, ret);
			return ret;
		}

		/* Authentic, but left by an earlier format. */
		if (UBI_HEADER_OK != headers.ec_status ||
		    headers.ec.image_seq != ubi->image_seq) {
			LOG_DBG("PEB %u: belongs to image 0x%08x, not 0x%08x",
				pnum, headers.ec.image_seq, ubi->image_seq);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		/* Damage this image left stays where it is. */
		if (UBI_PEB_CORRUPT == state)
			continue;

		/* Stamped for this image and still empty: allocatable. */
		if (UBI_HEADER_ERASED == headers.vid_status)
			continue;

		if (UBI_HEADER_OK != headers.vid_status) {
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		ret = scan_claim(ubi, pnum, &headers.vid);

		if (0 != ret)
			return ret;
	}

	return 0;
}
