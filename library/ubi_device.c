/**
 * \file    ubi_device.c
 * \author  Kamil Kielbasa
 * \brief   Formatting a partition and attaching to it.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_key.h"
#include "ubi_device.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_state.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Draws allowed before a random source that keeps returning zero gives up. */
#define IMAGE_SEQ_DRAW_LIMIT (4)

/** Sentinel for "no copy of the volume table was adopted". */
#define VOLUME_TABLE_COPY_NONE (UINT32_MAX)

/** Bytes read at a time when looking for anything left in a data area. */
#define SCAN_CHUNK (128)

/** Share of the partition that may be corrupted before attach gives up,
 *  and the floor Linux UBI applies to it on a small partition. */
#define CORRUPT_PEB_SHARE (20)
#define CORRUPT_PEB_FLOOR (8)

BUILD_ASSERT(UBI_LEB_UNMAPPED == UINT16_MAX,
	     "the unmapped sentinel must be all ones, so that memset sets it");

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

/** \name Opening and closing a partition */
/**@{*/

/**
 * \brief Ask the flash driver for the dimensions of the partition.
 */
static int device_geometry_measure(const struct flash_area *flash_area,
				   struct ubi_geometry *geometry);

/**
 * \brief Report whether this build can manage those dimensions.
 */
static int device_geometry_validate(const struct ubi_geometry *geometry);

/**
 * \brief Open the partition, measure it and derive the device keys.
 *
 *        The handle is left usable for physical access and for anything that
 *        only needs the keys; the per-block bookkeeping is not part of it,
 *        because formatting has no use for it.
 *
 *        \c ubi->geometry holds what was measured whenever measuring
 *        succeeded, even if the dimensions were then rejected, so that the
 *        caller can name the offending numbers.
 */
static int device_open(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Destroy the derived keys and close the partition.
 */
static void device_close(struct ubi_device *ubi);

/**
 * \brief Take the per-block bookkeeping from the heap.
 */
static int device_tables_alloc(struct ubi_device *ubi);

/**
 * \brief Give it back.
 */
static void device_tables_free(struct ubi_device *ubi);

/**@}*/

/** \name Talking to the application */
/**@{*/

/**@}*/

/** \name Reading the volume table */
/**@{*/

/**
 * \brief Draw an image sequence number that is not zero.
 */
static int device_image_seq_draw(uint32_t *image_seq);

/**
 * \brief Report whether every copy of the record says the same thing, so that
 *        losing any one of them costs nothing.
 */
static bool
device_volume_table_copies_agree(const struct volume_table_copy *copy,
				 size_t count,
				 const struct ubi_volume_table_record *record);

/**@}*/

/** \name Scanning the partition */
/**@{*/

/**
 * \brief Classify a block whose erase counter header verified but whose
 *        volume identifier header did not.
 *
 *        A blank data area means a write that was cut short, which is
 *        ordinary and safe to erase. Anything else is damage of unknown
 *        origin: Linux UBI preserves those blocks rather than destroying
 *        what they still hold, and so does this.
 *
 * \return #UBI_PEB_UNKNOWN, #UBI_PEB_CORRUPT or #UBI_PEB_BAD.
 */
static enum ubi_peb_state device_damaged_state(const struct ubi_device *ubi,
					       uint32_t pnum);

/**
 * \brief Refuse a partition that carries more damage than it can be worth
 *        attaching.
 *
 * \retval 0
 *         Few enough blocks are corrupted to carry on.
 * \retval -EINVAL
 *         Too many, and the partition is probably not what it looks like.
 */
static int device_corruption_check(const struct ubi_device *ubi);

/**
 * \brief Act on what the erase counter header turned out to be.
 *
 * \param[out] unopenable               Bumped when the header is well formed
 *                                      but will not verify under this key.
 *
 * \return Whether the block is worth looking at any further.
 */
static bool device_scan_ec(struct ubi_device *ubi, uint32_t pnum,
			   const struct ubi_headers *headers,
			   uint32_t *unopenable);

/**
 * \brief Act on what the volume identifier header turned out to be.
 *
 * \return Whether the block backs a logical erase block.
 */
static bool device_scan_vid(struct ubi_device *ubi, uint32_t pnum,
			    const struct ubi_headers *headers);

/**
 * \brief Remember which block holds each copy of the volume table record.
 */
static void device_scan_volume_table(struct ubi_device *ubi, uint32_t pnum,
				     const struct ubi_vid_header *vid);

/**
 * \brief Classify one block by the headers it carries.
 *
 *        Cannot fail: a block that will not say what it holds is recorded as
 *        unusable, which is an answer. Attach must survive a damaged block,
 *        so every path here ends in a state and an event, never an error.
 */
static void device_scan_peb(struct ubi_device *ubi, uint32_t pnum,
			    uint32_t *unopenable);

/**
 * \brief Decide which of two blocks claiming the same logical block is the
 *        older copy.
 *
 *        Linux UBI settles the same question in \c ubi_compare_lebs().
 *
 * \return The block to queue for reclaim.
 */
static uint32_t device_leb_older(const struct ubi_device *ubi,
				 uint32_t incumbent, uint32_t pnum,
				 uint64_t sqnum);

/**
 * \brief First of two passes: classify every block and find the volume table.
 *
 *        Runs before the volume table has been read, so it cannot yet tell
 *        which image a block belongs to nor which logical block it backs. It
 *        judges each block by its headers alone, records the erase counts and
 *        the highest sequence number, and fills in which block holds each
 *        copy of the volume table record.
 *
 *        \p unopenable counts blocks whose header is well formed but whose
 *        MAC does not verify: what separates a wrong key from a partition
 *        that was never formatted.
 *
 *        The caller must adopt one of those records, and set \c image_seq and
 *        the volumes from it, before running the second pass.
 */
static void device_scan_first_pass(struct ubi_device *ubi,
				   uint32_t *unopenable);

/**
 * \brief Second of two passes: apply what the volume table settled.
 *
 *        Revisits the #UBI_PEB_FREE and #UBI_PEB_MAPPED blocks the first pass
 *        left behind: those stamped for another image drop to
 *        #UBI_PEB_UNKNOWN, and the rest are hung off the logical blocks their
 *        VID headers name. The volume table's own blocks are left alone,
 *        because the first pass had to settle them before the record they
 *        hold could be read.
 */
static void device_scan_second_pass(struct ubi_device *ubi);

/**@}*/

/* Static function definitions --------------------------------------------- */

static int device_geometry_measure(const struct flash_area *flash_area,
				   struct ubi_geometry *geometry)
{
	const struct device *device = flash_area_get_device(flash_area);

	if (NULL == device)
		return -EIO;

	struct flash_pages_info page = { 0 };
	int ret =
		flash_get_page_info_by_offs(device, flash_area->fa_off, &page);

	if (0 != ret)
		return -EIO;

	const struct flash_parameters *parameters =
		flash_get_parameters(device);

	if (NULL == parameters)
		return -EIO;

	if (UBI_DATA_OFFSET >= page.size)
		return -EINVAL;

	const struct ubi_geometry measured = {
		.peb_count = flash_area->fa_size / page.size,
		.peb_size = page.size,
		.leb_size = page.size - UBI_DATA_OFFSET,
		.write_block_size = parameters->write_block_size,
		.erase_value = parameters->erase_value,
	};

	*geometry = measured;

	return 0;
}

static int device_geometry_validate(const struct ubi_geometry *geometry)
{
	/* Every volume this build allows has to fit one logical block, and
	 * every write has to land on a whole number of write blocks. */
	if (UBI_VOLUME_TABLE_RECORD_MAX_SIZE > geometry->leb_size)
		return -EINVAL;

	if (0 == geometry->write_block_size ||
	    0 != UBI_HEADER_SIZE % geometry->write_block_size)
		return -EINVAL;

	if (UBI_MIN_PEB_COUNT > geometry->peb_count)
		return -EINVAL;

	if (UBI_MAX_PEB_COUNT < geometry->peb_count)
		return -ENOSPC;

	return 0;
}

static int device_open(struct ubi_device *ubi, const struct ubi_config *config)
{
	const struct flash_area *flash_area = NULL;
	int ret = flash_area_open(config->flash_area_id, &flash_area);

	if (0 != ret)
		return -EIO;

	ubi->flash_area = flash_area;

	ret = device_geometry_measure(flash_area, &ubi->geometry);

	if (0 != ret) {
		flash_area_close(flash_area);
		ubi->flash_area = NULL;
		return ret;
	}

	ret = device_geometry_validate(&ubi->geometry);

	if (0 != ret) {
		flash_area_close(flash_area);
		ubi->flash_area = NULL;
		return ret;
	}

	ret = ubi_impl_key_derive(config->ikm_key_id, &ubi->keys.header,
				  &ubi->keys.volume_table);

	if (0 != ret) {
		flash_area_close(flash_area);
		ubi->flash_area = NULL;
		return ret;
	}

	/* The volume holding the record has to be reachable before the record
	 * declares anything, so it is set up here rather than built from it. */
	ubi->volume_table.volume.vol_id = UBI_VOLUME_TABLE_VOL_ID;
	ubi->volume_table.volume.leb_count = UBI_VOLUME_TABLE_LEB_COUNT;
	ubi->volume_table.volume.eba = ubi->volume_table.eba;
	strcpy(ubi->volume_table.volume.name, UBI_VOLUME_TABLE_NAME);

	memset(ubi->volume_table.eba, 0xFF, sizeof(ubi->volume_table.eba));

	return 0;
}

static void device_close(struct ubi_device *ubi)
{
	ubi_impl_key_destroy(&ubi->keys.header);
	ubi_impl_key_destroy(&ubi->keys.volume_table);
	flash_area_close(ubi->flash_area);
	ubi->flash_area = NULL;
}

static int device_tables_alloc(struct ubi_device *ubi)
{
	const uint32_t peb_count = ubi->geometry.peb_count;

	ubi->blocks.state = k_calloc(peb_count, sizeof(uint8_t));
	ubi->blocks.erase_count = k_calloc(peb_count, sizeof(uint32_t));
	ubi->blocks.protect = k_calloc(peb_count, sizeof(uint8_t));
	ubi->volumes.eba_pool = k_calloc(peb_count, sizeof(uint16_t));

	if (NULL == ubi->blocks.state || NULL == ubi->blocks.erase_count ||
	    NULL == ubi->blocks.protect || NULL == ubi->volumes.eba_pool) {
		device_tables_free(ubi);
		return -ENOMEM;
	}

	/* A zeroed state is UBI_PEB_UNKNOWN, but an unmapped entry is all
	 * ones. */
	memset(ubi->volumes.eba_pool, 0xFF, peb_count * sizeof(uint16_t));

	return 0;
}

static void device_tables_free(struct ubi_device *ubi)
{
	k_free(ubi->blocks.state);
	k_free(ubi->blocks.erase_count);
	k_free(ubi->blocks.protect);
	k_free(ubi->volumes.eba_pool);

	ubi->blocks.state = NULL;
	ubi->blocks.erase_count = NULL;
	ubi->blocks.protect = NULL;
	ubi->volumes.eba_pool = NULL;
}

static int device_image_seq_draw(uint32_t *image_seq)
{
	/* Zero marks "no image", so a fresh one must not land on it. */
	for (uint32_t attempt = 0; attempt < IMAGE_SEQ_DRAW_LIMIT; ++attempt) {
		uint32_t drawn = 0;
		const psa_status_t status =
			psa_generate_random((uint8_t *)&drawn, sizeof(drawn));

		if (PSA_SUCCESS != status)
			return -EIO;

		if (0 != drawn) {
			*image_seq = drawn;
			return 0;
		}
	}

	return -EIO;
}

static bool
device_volume_table_copies_agree(const struct volume_table_copy *copy,
				 size_t count,
				 const struct ubi_volume_table_record *record)
{
	for (size_t i = 0; i < count; ++i) {
		if (!copy[i].readable ||
		    copy[i].image_seq != record->image_seq ||
		    copy[i].revision != record->revision)
			return false;
	}

	return true;
}

static enum ubi_peb_state device_damaged_state(const struct ubi_device *ubi,
					       uint32_t pnum)
{
	uint8_t chunk[SCAN_CHUNK];

	for (uint32_t at = 0; at < ubi->geometry.leb_size;
	     at += sizeof(chunk)) {
		const uint32_t length =
			MIN(sizeof(chunk), ubi->geometry.leb_size - at);

		if (0 != ubi_impl_io_read_data(ubi, pnum, at, chunk, length))
			return UBI_PEB_BAD;

		for (uint32_t i = 0; i < length; ++i) {
			if (ubi->geometry.erase_value != chunk[i])
				return UBI_PEB_CORRUPT;
		}
	}

	return UBI_PEB_UNKNOWN;
}

static int device_corruption_check(const struct ubi_device *ubi)
{
	const uint32_t allowed = MAX(
		ubi->geometry.peb_count / CORRUPT_PEB_SHARE, CORRUPT_PEB_FLOOR);
	uint32_t corrupted = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		if (UBI_PEB_CORRUPT == ubi_impl_peb_state_get(ubi, pnum))
			corrupted += 1;
	}

	if (corrupted < allowed)
		return 0;

	LOG_ERR("%u of %u blocks are corrupted, more than the %u this "
		"partition can be carried with",
		corrupted, ubi->geometry.peb_count, allowed);

	return -EINVAL;
}

static bool device_scan_ec(struct ubi_device *ubi, uint32_t pnum,
			   const struct ubi_headers *headers,
			   uint32_t *unopenable)
{
	switch (headers->ec_status) {
	case UBI_HEADER_OK:
		break;
	case UBI_HEADER_ERASED:
	case UBI_HEADER_NOT_UBI:
		/* Blank, or someone else's bytes; erase before use. */
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	case UBI_HEADER_CORRUPT:
		/* An interrupted stamp, so the block itself is fine. */
		ubi_impl_event_emit(ubi, UBI_EVENT_HDR_CORRUPT, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	case UBI_HEADER_TAMPERED:
		/* A well-formed header under a key this build does not hold;
		 * counting those apart keeps a mistyped key from looking like
		 * a blank partition. */
		*unopenable += 1;
		ubi_impl_event_emit(ubi, UBI_EVENT_HDR_TAMPERED, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
		return false;
	case UBI_HEADER_ERROR:
	default:
		ubi_impl_event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
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

static bool device_scan_vid(struct ubi_device *ubi, uint32_t pnum,
			    const struct ubi_headers *headers)
{
	/* The erase counter header verified under this key, so damage behind
	 * it condemns one block, not the whole device. */
	switch (headers->vid_status) {
	case UBI_HEADER_OK:
		return true;
	case UBI_HEADER_ERASED:
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_FREE);
		return false;
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
	case UBI_HEADER_ERROR:
	default:
		ubi_impl_event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				    UBI_VOL_ID_INVALID, 0);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
		return false;
	}

	ubi_impl_peb_state_set(ubi, pnum, device_damaged_state(ubi, pnum));

	return false;
}

static void device_scan_volume_table(struct ubi_device *ubi, uint32_t pnum,
				     const struct ubi_vid_header *vid)
{
	/* Left mapped for the second pass to queue for reclaim. */
	if (vid->lnum >= UBI_VOLUME_TABLE_LEB_COUNT) {
		LOG_WRN("PEB %u: claims volume table copy %u, of which there "
			"are only %d",
			pnum, vid->lnum, UBI_VOLUME_TABLE_LEB_COUNT);
		return;
	}

	/*
	 * Several formats can have left records behind, so the newest sequence
	 * number wins. It is the fresh one by construction: a format starts
	 * its numbering above everything it finds. The blocks that lose are
	 * left as they are, for the second pass to judge once the image is
	 * known.
	 */
	if (UBI_LEB_UNMAPPED != ubi->volume_table.eba[vid->lnum] &&
	    vid->sqnum <= ubi->volume_table.sqnum[vid->lnum]) {
		LOG_DBG("PEB %u: holds a superseded volume table copy %u", pnum,
			vid->lnum);
		return;
	}

	ubi->volume_table.eba[vid->lnum] = (uint16_t)pnum;
	ubi->volume_table.sqnum[vid->lnum] = vid->sqnum;
}

static void device_scan_peb(struct ubi_device *ubi, uint32_t pnum,
			    uint32_t *unopenable)
{
	struct ubi_headers headers = { 0 };

	if (0 != ubi_impl_header_read(ubi, pnum, &headers)) {
		LOG_WRN("PEB %u: unreadable, retiring it", pnum);
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
		ubi_impl_event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				    UBI_VOL_ID_INVALID, 0);
		return;
	}

	if (!device_scan_ec(ubi, pnum, &headers, unopenable))
		return;

	ubi->blocks.erase_count[pnum] = (uint32_t)headers.ec.erase_count;

	if (!device_scan_vid(ubi, pnum, &headers))
		return;

	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

	if (headers.vid.sqnum > ubi->global_sqnum)
		ubi->global_sqnum = headers.vid.sqnum;

	if (UBI_VOLUME_TABLE_VOL_ID == headers.vid.vol_id)
		device_scan_volume_table(ubi, pnum, &headers.vid);
}

static uint32_t device_leb_older(const struct ubi_device *ubi,
				 uint32_t incumbent, uint32_t pnum,
				 uint64_t sqnum)
{
	struct ubi_headers other = { 0 };
	uint64_t incumbent_sqnum = 0;

	/* A block that will not say how old it is loses to one that will. */
	if (0 == ubi_impl_header_read(ubi, incumbent, &other) &&
	    UBI_HEADER_OK == other.vid_status)
		incumbent_sqnum = other.vid.sqnum;

	return (incumbent_sqnum > sqnum) ? pnum : incumbent;
}

static void device_scan_first_pass(struct ubi_device *ubi, uint32_t *unopenable)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum)
		device_scan_peb(ubi, pnum, unopenable);
}

static void device_scan_second_pass(struct ubi_device *ubi)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_FREE != state && UBI_PEB_MAPPED != state)
			continue;

		struct ubi_headers headers = { 0 };
		int ret = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != ret) {
			LOG_WRN("PEB %u: became unreadable, retiring it", pnum);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
			continue;
		}

		if (UBI_HEADER_OK != headers.ec_status) {
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		if (headers.ec.image_seq != ubi->image_seq) {
			/* Authentic, but left by an earlier format: Linux
			 * refuses the whole attach, we reclaim the block. */
			LOG_DBG("PEB %u: belongs to image 0x%08x, not 0x%08x",
				pnum, headers.ec.image_seq, ubi->image_seq);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		/* Stamped for this image and still empty: allocatable. */
		if (UBI_HEADER_ERASED == headers.vid_status)
			continue;

		if (UBI_HEADER_OK != headers.vid_status) {
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		const struct ubi_vid_header *vid = &headers.vid;

		if (UBI_VOLUME_TABLE_VOL_ID == vid->vol_id) {
			/* The first pass settled which block holds each copy;
			 * every other claimant is a superseded one. */
			if (vid->lnum < UBI_VOLUME_TABLE_LEB_COUNT &&
			    pnum == ubi->volume_table.eba[vid->lnum])
				continue;

			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
			continue;
		}

		uint16_t incumbent = UBI_LEB_UNMAPPED;

		ret = ubi_impl_volume_leb_get(ubi, vid->vol_id, vid->lnum,
					      &incumbent);

		if (0 != ret) {
			LOG_WRN("PEB %u: claims volume %u block %u, which the "
				"volume table does not describe (%d); queued "
				"for reclaim",
				pnum, vid->vol_id, vid->lnum, ret);
			ubi_impl_event_emit(ubi, UBI_EVENT_LEB_ORPHANED, pnum,
					    vid->vol_id, vid->lnum);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
			continue;
		}

		/* A sealed header promises a length and a checksum over the
		 * data behind it, so a write cut short by a power loss is
		 * recognisable here and must not win the block. */
		if (0 != ubi_impl_header_vid_data_verify(ubi, pnum, vid)) {
			LOG_WRN("PEB %u: claims volume %u block %u but its data "
				"was cut short; queued for reclaim",
				pnum, vid->vol_id, vid->lnum);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
			continue;
		}

		if (UBI_LEB_UNMAPPED != incumbent) {
			const uint32_t stale = device_leb_older(
				ubi, incumbent, pnum, vid->sqnum);

			LOG_WRN("volume %u block %u is claimed by PEB %u and "
				"PEB %u; PEB %u is the older copy",
				vid->vol_id, vid->lnum, incumbent, pnum, stale);

			ubi_impl_peb_state_set(ubi, stale, UBI_PEB_RECLAIM);

			if (stale == pnum)
				continue;
		}

		ret = ubi_impl_volume_leb_set(ubi, vid->vol_id, vid->lnum,
					      (uint16_t)pnum);

		if (0 != ret) {
			LOG_WRN("PEB %u: volume %u block %u would not take it "
				"(%d); queued for reclaim",
				pnum, vid->vol_id, vid->lnum, ret);
			ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
		}
	}
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_device_format(const struct ubi_config *config)
{
	int ret = 0;

	/*
	 * Formatting has no attached device, but it needs the same partition,
	 * geometry and keys, so it borrows a handle without the per-block
	 * bookkeeping, which it has no use for.
	 */
	struct ubi_device *ubi = k_calloc(1, sizeof(*ubi));

	if (NULL == ubi) {
		LOG_ERR("no memory for a device handle of %zu bytes",
			sizeof(*ubi));
		return -ENOMEM;
	}

	ret = device_open(ubi, config);

	if (0 != ret) {
		LOG_ERR("cannot open partition %u (%d): %u blocks of %u bytes, "
			"write granularity %u",
			config->flash_area_id, ret, ubi->geometry.peb_count,
			ubi->geometry.peb_size, ubi->geometry.write_block_size);
		k_free(ubi);
		return ret;
	}

	ret = device_image_seq_draw(&ubi->image_seq);

	if (0 != ret) {
		LOG_ERR("cannot draw an image sequence number for partition %u",
			config->flash_area_id);
		device_close(ubi);
		k_free(ubi);
		return ret;
	}

	/*
	 * Sequence numbers have to outrank everything already on the flash.
	 * A format leaves most blocks untouched, so an old volume table with
	 * a higher number would otherwise win the next attach and quietly
	 * undo the format.
	 */
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		struct ubi_headers headers = { 0 };

		ret = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != ret)
			continue;

		if (UBI_HEADER_OK == headers.vid_status &&
		    headers.vid.sqnum > ubi->global_sqnum)
			ubi->global_sqnum = headers.vid.sqnum;
	}

	const struct ubi_volume_table_record record = {
		.revision = 1,
		.image_seq = ubi->image_seq,
		.peb_size = ubi->geometry.peb_size,
		.peb_count = ubi->geometry.peb_count,
	};

	uint32_t written = 0;

	/*
	 * Both copies go down now, so that the invariant updates rely on - one
	 * complete copy always survives - holds from the first second of the
	 * device's life. Blocks that cannot take one are skipped, which is the
	 * only reason this walks the partition rather than taking the first
	 * two blocks.
	 */
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count &&
				written < UBI_VOLUME_TABLE_LEB_COUNT;
	     ++pnum) {
		ret = ubi_impl_peb_prepare(ubi, pnum);

		if (0 != ret) {
			LOG_WRN("PEB %u cannot be prepared (%d), trying the "
				"next one",
				pnum, ret);
			continue;
		}

		ubi->volume_table.eba[written] = (uint16_t)pnum;

		ret = ubi_impl_volume_table_write(ubi, written, &record);

		if (0 != ret) {
			LOG_WRN("PEB %u cannot hold a volume table copy (%d), "
				"trying the next one",
				pnum, ret);
			ubi->volume_table.eba[written] = UBI_LEB_UNMAPPED;
			continue;
		}

		written += 1;
	}

	if (UBI_VOLUME_TABLE_LEB_COUNT != written) {
		LOG_ERR("partition %u: only %u of the %d volume table copies "
			"could be written",
			config->flash_area_id, written,
			UBI_VOLUME_TABLE_LEB_COUNT);
		device_close(ubi);
		k_free(ubi);
		return -ENOSPC;
	}

	LOG_INF("formatted partition %u of %u blocks: image_seq=0x%08x, "
		"volume table in PEB %u and PEB %u",
		config->flash_area_id, ubi->geometry.peb_count, ubi->image_seq,
		ubi->volume_table.eba[0], ubi->volume_table.eba[1]);

	device_close(ubi);
	k_free(ubi);

	return 0;
}

int ubi_impl_device_init(struct ubi_device *ubi,
			 const struct ubi_config *config)
{
	int ret = 0;

	memset(ubi, 0, sizeof(*ubi));
	k_mutex_init(&ubi->lock);

	ubi->callbacks.event = config->event_cb;
	ubi->callbacks.state = config->state_cb;
	ubi->callbacks.user_context = config->user_context;

	ret = device_open(ubi, config);

	if (0 != ret) {
		LOG_ERR("cannot open partition %u (%d): %u blocks of %u bytes, "
			"write granularity %u",
			config->flash_area_id, ret, ubi->geometry.peb_count,
			ubi->geometry.peb_size, ubi->geometry.write_block_size);
		return ret;
	}

	ret = device_tables_alloc(ubi);

	if (0 != ret) {
		LOG_ERR("no memory for the bookkeeping of %u erase blocks",
			ubi->geometry.peb_count);
		device_close(ubi);
		return ret;
	}

	uint32_t unopenable = 0;

	device_scan_first_pass(ubi, &unopenable);

	ret = device_corruption_check(ubi);

	if (0 != ret) {
		ubi_impl_device_deinit(ubi);
		return ret;
	}

	/*
	 * Oldest copy first, so that the record left in hand is the newest
	 * one that reads back. Both are read either way: two copies that
	 * disagree leave the device one erase away from a silent rollback,
	 * and the application has to hear about that.
	 */
	const uint32_t newest =
		(ubi->volume_table.sqnum[0] >= ubi->volume_table.sqnum[1]) ? 0 :
									     1;
	const uint32_t order[UBI_VOLUME_TABLE_LEB_COUNT] = { 1 - newest,
							     newest };
	struct ubi_volume_table_record record = { 0 };
	struct volume_table_copy copy[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t adopted = VOLUME_TABLE_COPY_NONE;
	uint32_t found = 0;

	for (uint32_t i = 0; i < UBI_VOLUME_TABLE_LEB_COUNT; ++i) {
		const uint32_t lnum = order[i];
		const uint32_t pnum = ubi->volume_table.eba[lnum];

		if (UBI_LEB_UNMAPPED == pnum)
			continue;

		found += 1;

		ret = ubi_impl_volume_table_read(ubi, lnum, &record);

		if (0 != ret) {
			ubi_impl_event_emit(ubi, UBI_EVENT_VOLUME_TABLE_CORRUPT,
					    pnum, UBI_VOLUME_TABLE_VOL_ID,
					    lnum);
			LOG_ERR("PEB %u holds volume table copy %u and it is "
				"unusable (%d)",
				pnum, lnum, ret);
			continue;
		}

		copy[lnum].readable = true;
		copy[lnum].image_seq = record.image_seq;
		copy[lnum].revision = record.revision;
		adopted = lnum;
	}

	if (VOLUME_TABLE_COPY_NONE == adopted) {
		ret = -ENODEV;

		if (0 != found) {
			LOG_ERR("partition %u: none of its %u volume table "
				"copies could be read",
				config->flash_area_id, found);
		} else if (0 != unopenable) {
			LOG_ERR("partition %u: %u blocks carry UBI headers "
				"that will not verify; the key is wrong or "
				"the metadata was modified",
				config->flash_area_id, unopenable);
			ret = -EBADMSG;
		} else {
			LOG_INF("partition %u holds no UBI device",
				config->flash_area_id);
		}

		goto exit;
	}

	if (!device_volume_table_copies_agree(copy, ARRAY_SIZE(copy),
					      &record)) {
		LOG_WRN("partition %u: not every copy of the volume table is "
			"current, so one erase would cost a revision",
			config->flash_area_id);
		ubi->volume_table.degraded = true;
		ubi_impl_event_emit(ubi, UBI_EVENT_VOLUME_TABLE_DEGRADED,
				    ubi->volume_table.eba[adopted],
				    UBI_VOLUME_TABLE_VOL_ID, adopted);
	}

	if (record.peb_size != ubi->geometry.peb_size ||
	    record.peb_count != ubi->geometry.peb_count) {
		LOG_ERR("partition %u is %u blocks of %u bytes, the volume "
			"table was written for %u of %u",
			config->flash_area_id, ubi->geometry.peb_count,
			ubi->geometry.peb_size, record.peb_count,
			record.peb_size);
		ret = -EINVAL;
		goto exit;
	}

	ubi->image_seq = record.image_seq;
	ubi->volumes.revision = record.revision;
	ubi->volume_table.current = adopted;

	ret = ubi_impl_volumes_build(ubi, &record);

	if (0 != ret) {
		LOG_ERR("the volume table declares more logical blocks than "
			"the %u this partition has",
			ubi->geometry.peb_count);
		goto exit;
	}

	device_scan_second_pass(ubi);

	ret = ubi_impl_state_check(ubi);

	if (0 != ret) {
		LOG_ERR("partition %u: the state check refused this device",
			config->flash_area_id);
		goto exit;
	}

	ubi->magic = UBI_DEVICE_MAGIC;

	LOG_INF("attached partition %u: image_seq=0x%08x, %u volumes, "
		"revision %u",
		config->flash_area_id, ubi->image_seq, ubi->volumes.count,
		ubi->volumes.revision);

	return 0;

exit:
	device_tables_free(ubi);
	device_close(ubi);
	memset(ubi, 0, sizeof(*ubi));

	return ret;
}

void ubi_impl_device_deinit(struct ubi_device *ubi)
{
	device_tables_free(ubi);
	device_close(ubi);

	memset(ubi, 0, sizeof(*ubi));
}
