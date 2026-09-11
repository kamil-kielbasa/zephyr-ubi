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
#include "ubi_private.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Draws allowed before a random source that keeps returning zero gives up. */
#define IMAGE_SEQ_DRAW_LIMIT (4)

/** Sentinel for "no copy of the volume table was adopted". */
#define VOLUME_TABLE_COPY_NONE (UINT32_MAX)

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
 * \brief Reject a configuration that cannot drive the library.
 */
static int config_validate(const struct ubi_config *config);

/**
 * \brief Ask the flash driver for the dimensions of the partition.
 */
static int geometry_measure(const struct flash_area *flash_area,
			    struct ubi_geometry *geometry);

/**
 * \brief Report whether this build can manage those dimensions.
 */
static int geometry_validate(const struct ubi_geometry *geometry);

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

/**
 * \brief Hand an event to the application.
 */
static void event_emit(const struct ubi_device *ubi, enum ubi_event_type type,
		       uint32_t pnum, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Take stock of the device as it stands. Reports \c -EFAULT when a
 *        block carries a state no \ref ubi_peb_state names, because the
 *        counts would be a lie and the table itself is not to be trusted.
 */
static int device_info_fill(const struct ubi_device *ubi,
			    struct ubi_device_info *info);

/**
 * \brief Ask the application whether it still trusts the device, and latch a
 *        refusal so that retrying is not a way around it.
 */
static int state_check(struct ubi_device *ubi);

/**@}*/

/** \name Block state and preparation */
/**@{*/

/**
 * \brief Read the lifecycle state of a physical erase block.
 */
static enum ubi_peb_state peb_state_get(const struct ubi_device *ubi,
					uint32_t pnum);

/**
 * \brief Set it. The narrowing to a byte happens here and nowhere else.
 */
static void peb_state_set(struct ubi_device *ubi, uint32_t pnum,
			  enum ubi_peb_state state);

/**
 * \brief Draw an image sequence number that is not zero.
 */
static int image_seq_draw(uint32_t *image_seq);

/**
 * \brief Erase a block and stamp it with a fresh erase counter header.
 *
 *        Wear history is worth keeping, so the previous count is read back
 *        first; a block whose count can no longer be trusted starts again
 *        from zero, and the caller is told so it can say as much.
 */
static int peb_prepare(const struct ubi_device *ubi, uint32_t pnum,
		       bool *history_lost);

/**@}*/

/** \name Rebuilding the volumes */
/**@{*/

/**
 * \brief Give every declared volume its slice of the mapping table.
 */
static int volumes_build(struct ubi_device *ubi,
			 const struct ubi_volume_table_record *record);

/**
 * \brief Look up a volume by identifier, including the internal one that
 *        holds the volume table. Public entry points must keep the latter
 *        out of the application's reach.
 */
static struct ubi_volume *volume_find(struct ubi_device *ubi, uint32_t vol_id);

/**
 * \brief Report which physical block backs a logical one, or
 *        #UBI_LEB_UNMAPPED when none does. Returns \c -ENOENT for an unknown
 *        volume and \c -ERANGE for a volume that does not reach that far.
 */
static int leb_mapping_get(const struct ubi_device *ubi, uint32_t vol_id,
			   uint32_t lnum, uint16_t *pnum);

/**
 * \brief Point a logical block at a physical one, with the same contract.
 */
static int leb_mapping_set(struct ubi_device *ubi, uint32_t vol_id,
			   uint32_t lnum, uint16_t pnum);

/**
 * \brief Report whether every copy of the record says the same thing, so that
 *        losing any one of them costs nothing.
 */
static bool
volume_table_copies_agree(const struct volume_table_copy *copy, size_t count,
			  const struct ubi_volume_table_record *record);

/**@}*/

/** \name Scanning the partition */
/**@{*/

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
 *        tag does not verify: what separates a wrong key from a partition
 *        that was never formatted.
 *
 *        The caller must adopt one of those records, and set \c image_seq and
 *        the volumes from it, before running the second pass.
 */
static void scan_first_pass(struct ubi_device *ubi, uint32_t *unopenable);

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
static void scan_second_pass(struct ubi_device *ubi);

/**@}*/

/* Static function definitions --------------------------------------------- */

static int config_validate(const struct ubi_config *config)
{
	if (NULL == config)
		return -EINVAL;

	if (PSA_KEY_ID_NULL == config->ikm_key_id)
		return -EINVAL;

	if (NULL == config->event_cb || NULL == config->state_cb)
		return -EINVAL;

	return 0;
}

static int geometry_measure(const struct flash_area *flash_area,
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

static int geometry_validate(const struct ubi_geometry *geometry)
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

	ret = geometry_measure(flash_area, &ubi->geometry);

	if (0 != ret) {
		flash_area_close(flash_area);
		ubi->flash_area = NULL;
		return ret;
	}

	ret = geometry_validate(&ubi->geometry);

	if (0 != ret) {
		flash_area_close(flash_area);
		ubi->flash_area = NULL;
		return ret;
	}

	ret = ubi_key_derive(config->ikm_key_id, &ubi->keys.header,
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
	ubi_key_destroy(&ubi->keys.header);
	ubi_key_destroy(&ubi->keys.volume_table);
	flash_area_close(ubi->flash_area);
	ubi->flash_area = NULL;
}

static int device_tables_alloc(struct ubi_device *ubi)
{
	const uint32_t peb_count = ubi->geometry.peb_count;

	ubi->blocks.state = k_calloc(peb_count, sizeof(uint8_t));
	ubi->blocks.erase_count = k_calloc(peb_count, sizeof(uint32_t));
	ubi->volumes.eba_pool = k_calloc(peb_count, sizeof(uint16_t));

	if (NULL == ubi->blocks.state || NULL == ubi->blocks.erase_count ||
	    NULL == ubi->volumes.eba_pool) {
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
	k_free(ubi->volumes.eba_pool);

	ubi->blocks.state = NULL;
	ubi->blocks.erase_count = NULL;
	ubi->volumes.eba_pool = NULL;
}

static void event_emit(const struct ubi_device *ubi, enum ubi_event_type type,
		       uint32_t pnum, uint32_t vol_id, uint32_t lnum)
{
	const struct ubi_event event = {
		.type = type,
		.pnum = pnum,
		.vol_id = vol_id,
		.lnum = lnum,
	};

	ubi->callbacks.event(&event, ubi->callbacks.user_context);
}

static int device_info_fill(const struct ubi_device *ubi,
			    struct ubi_device_info *info)
{
	struct ubi_device_info measured = {
		.peb_count = ubi->geometry.peb_count,
		.peb_size = ubi->geometry.peb_size,
		.leb_size = ubi->geometry.leb_size,
		.write_block_size = ubi->geometry.write_block_size,
		.volume_count = ubi->volumes.count,
		.image_seq = ubi->image_seq,
		.revision = ubi->volumes.revision,
		.global_sqnum = ubi->global_sqnum,
	};
	uint32_t lowest_erase_count = UINT32_MAX;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state = peb_state_get(ubi, pnum);
		const uint32_t erase_count = ubi->blocks.erase_count[pnum];

		switch (state) {
		case UBI_PEB_FREE:
			measured.free_pebs += 1;
			break;
		case UBI_PEB_UNKNOWN:
		case UBI_PEB_RECLAIM:
			measured.reclaimable_pebs += 1;
			break;
		case UBI_PEB_BAD:
			measured.bad_pebs += 1;
			break;
		case UBI_PEB_MAPPED:
			break;
		default:
			return -EFAULT;
		}

		/* Every other state means the erase counter header verified
		 * and named this image, so its count can be added up. */
		if (UBI_PEB_UNKNOWN == state || UBI_PEB_BAD == state)
			continue;

		measured.healthy_pebs += 1;
		measured.total_erase_count += erase_count;

		if (erase_count < lowest_erase_count)
			lowest_erase_count = erase_count;

		if (erase_count > measured.max_erase_count)
			measured.max_erase_count = erase_count;
	}

	if (0 != measured.healthy_pebs)
		measured.min_erase_count = lowest_erase_count;

	*info = measured;

	return 0;
}

static int state_check(struct ubi_device *ubi)
{
	if (ubi->untrusted)
		return -EROFS;

	struct ubi_device_info info = { 0 };
	const int ret = device_info_fill(ubi, &info);

	if (0 != ret)
		return ret;

	ubi->writes_since_check = 0;

	if (UBI_STATE_TRUSTED ==
	    ubi->callbacks.state(&info, ubi->callbacks.user_context))
		return 0;

	ubi->untrusted = true;

	return -EROFS;
}

static enum ubi_peb_state peb_state_get(const struct ubi_device *ubi,
					uint32_t pnum)
{
	return (enum ubi_peb_state)ubi->blocks.state[pnum];
}

static void peb_state_set(struct ubi_device *ubi, uint32_t pnum,
			  enum ubi_peb_state state)
{
	ubi->blocks.state[pnum] = (uint8_t)state;
}

static int image_seq_draw(uint32_t *image_seq)
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

static int peb_prepare(const struct ubi_device *ubi, uint32_t pnum,
		       bool *history_lost)
{
	struct ubi_headers headers = { 0 };
	int ret = ubi_headers_read(ubi, pnum, &headers);

	if (0 != ret)
		return ret;

	const bool trustworthy = (UBI_HEADER_OK == headers.ec_status) ||
				 (UBI_HEADER_ERASED == headers.ec_status);
	const uint64_t erase_count = (UBI_HEADER_OK == headers.ec_status) ?
					     headers.ec.erase_count :
					     0;

	*history_lost = !trustworthy;

	ret = ubi_io_erase(ubi, pnum);

	if (0 != ret)
		return ret;

	const struct ubi_ec_header header = {
		.erase_count = erase_count + 1,
		.image_seq = ubi->image_seq,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};

	return ubi_ec_header_write(ubi, pnum, &header);
}

static int volumes_build(struct ubi_device *ubi,
			 const struct ubi_volume_table_record *record)
{
	ubi->volumes.count = record->volume_count;
	ubi->volumes.id_watermark = record->vol_id_watermark;
	ubi->volumes.eba_used = 0;

	for (uint32_t i = 0; i < record->volume_count; ++i) {
		const struct ubi_volume_table_entry *entry =
			&record->entries[i];
		struct ubi_volume *volume = &ubi->volumes.entries[i];

		if (entry->leb_count >
		    ubi->geometry.peb_count - ubi->volumes.eba_used)
			return -ENOSPC;

		volume->vol_id = entry->vol_id;
		volume->leb_count = entry->leb_count;
		strncpy(volume->name, entry->name, sizeof(volume->name) - 1);
		volume->name[sizeof(volume->name) - 1] = '\0';
		volume->eba = &ubi->volumes.eba_pool[ubi->volumes.eba_used];

		ubi->volumes.eba_used += entry->leb_count;
	}

	return 0;
}

static struct ubi_volume *volume_find(struct ubi_device *ubi, uint32_t vol_id)
{
	if (UBI_VOLUME_TABLE_VOL_ID == vol_id)
		return &ubi->volume_table.volume;

	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		if (ubi->volumes.entries[i].vol_id == vol_id)
			return &ubi->volumes.entries[i];
	}

	return NULL;
}

static int leb_mapping_get(const struct ubi_device *ubi, uint32_t vol_id,
			   uint32_t lnum, uint16_t *pnum)
{
	const struct ubi_volume *volume =
		volume_find((struct ubi_device *)ubi, vol_id);

	if (NULL == volume)
		return -ENOENT;

	if (lnum >= volume->leb_count)
		return -ERANGE;

	*pnum = volume->eba[lnum];

	return 0;
}

static int leb_mapping_set(struct ubi_device *ubi, uint32_t vol_id,
			   uint32_t lnum, uint16_t pnum)
{
	struct ubi_volume *volume = volume_find(ubi, vol_id);

	if (NULL == volume)
		return -ENOENT;

	if (lnum >= volume->leb_count)
		return -ERANGE;

	volume->eba[lnum] = pnum;

	return 0;
}

static bool
volume_table_copies_agree(const struct volume_table_copy *copy, size_t count,
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

static void scan_first_pass(struct ubi_device *ubi, uint32_t *unopenable)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		struct ubi_headers headers = { 0 };
		const int ret = ubi_headers_read(ubi, pnum, &headers);

		if (0 != ret) {
			LOG_WRN("PEB %u: unreadable, retiring it", pnum);
			peb_state_set(ubi, pnum, UBI_PEB_BAD);
			event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				   UBI_VOL_ID_INVALID, 0);
			continue;
		}

		switch (headers.ec_status) {
		case UBI_HEADER_OK:
			break;
		case UBI_HEADER_ERASED:
		case UBI_HEADER_NOT_UBI:
			/* Blank, or someone else's bytes; erase before use. */
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		case UBI_HEADER_CORRUPT:
			/* An interrupted stamp, so the block itself is fine. */
			event_emit(ubi, UBI_EVENT_HDR_CORRUPT, pnum,
				   UBI_VOL_ID_INVALID, 0);
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		case UBI_HEADER_TAMPERED:
			/* A well-formed header under a key this build does not
			 * hold; counting those apart keeps a mistyped key from
			 * looking like a blank partition. */
			*unopenable += 1;
			event_emit(ubi, UBI_EVENT_HDR_TAMPERED, pnum,
				   UBI_VOL_ID_INVALID, 0);
			peb_state_set(ubi, pnum, UBI_PEB_BAD);
			continue;
		case UBI_HEADER_ERROR:
		default:
			event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				   UBI_VOL_ID_INVALID, 0);
			peb_state_set(ubi, pnum, UBI_PEB_BAD);
			continue;
		}

		ubi->blocks.erase_count[pnum] =
			(uint32_t)headers.ec.erase_count;

		/* The erase counter header verified under this key, so damage
		 * behind it condemns one block, not the whole device. */
		switch (headers.vid_status) {
		case UBI_HEADER_OK:
			break;
		case UBI_HEADER_ERASED:
			peb_state_set(ubi, pnum, UBI_PEB_FREE);
			continue;
		case UBI_HEADER_NOT_UBI:
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		case UBI_HEADER_CORRUPT:
			event_emit(ubi, UBI_EVENT_HDR_CORRUPT, pnum,
				   UBI_VOL_ID_INVALID, 0);
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		case UBI_HEADER_TAMPERED:
			event_emit(ubi, UBI_EVENT_HDR_TAMPERED, pnum,
				   UBI_VOL_ID_INVALID, 0);
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		case UBI_HEADER_ERROR:
		default:
			event_emit(ubi, UBI_EVENT_PEB_BAD, pnum,
				   UBI_VOL_ID_INVALID, 0);
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

		if (headers.vid.sqnum > ubi->global_sqnum)
			ubi->global_sqnum = headers.vid.sqnum;

		if (UBI_VOLUME_TABLE_VOL_ID != headers.vid.vol_id)
			continue;

		const uint32_t lnum = headers.vid.lnum;

		/* A block claiming a copy that does not exist is left for the
		 * second pass to queue for reclaim. */
		if (lnum >= UBI_VOLUME_TABLE_LEB_COUNT)
			continue;

		/*
		 * Several formats can have left records behind, so the newest
		 * sequence number wins. It is the fresh one by construction:
		 * a format starts its numbering above everything it finds.
		 * The blocks that lose are left as they are, for the second
		 * pass to judge once the image is known.
		 */
		if (UBI_LEB_UNMAPPED != ubi->volume_table.eba[lnum] &&
		    headers.vid.sqnum <= ubi->volume_table.sqnum[lnum])
			continue;

		ubi->volume_table.eba[lnum] = (uint16_t)pnum;
		ubi->volume_table.sqnum[lnum] = headers.vid.sqnum;
	}
}

static void scan_second_pass(struct ubi_device *ubi)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state = peb_state_get(ubi, pnum);

		if (UBI_PEB_FREE != state && UBI_PEB_MAPPED != state)
			continue;

		struct ubi_headers headers = { 0 };
		int ret = ubi_headers_read(ubi, pnum, &headers);

		if (0 != ret) {
			LOG_WRN("PEB %u: became unreadable, retiring it", pnum);
			peb_state_set(ubi, pnum, UBI_PEB_BAD);
			continue;
		}

		if (UBI_HEADER_OK != headers.ec_status) {
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		if (headers.ec.image_seq != ubi->image_seq) {
			/* Authentic, but left by an earlier format: Linux
			 * refuses the whole attach, we reclaim the block. */
			LOG_DBG("PEB %u: belongs to image 0x%08x, not 0x%08x",
				pnum, headers.ec.image_seq, ubi->image_seq);
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		/* Stamped for this image and still empty: allocatable. */
		if (UBI_HEADER_ERASED == headers.vid_status)
			continue;

		if (UBI_HEADER_OK != headers.vid_status) {
			peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);
			continue;
		}

		const struct ubi_vid_header *vid = &headers.vid;

		if (UBI_VOLUME_TABLE_VOL_ID == vid->vol_id) {
			/* The first pass settled which block holds each copy;
			 * every other claimant is a superseded one. */
			if (vid->lnum < UBI_VOLUME_TABLE_LEB_COUNT &&
			    pnum == ubi->volume_table.eba[vid->lnum])
				continue;

			peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
			continue;
		}

		uint16_t incumbent = UBI_LEB_UNMAPPED;

		ret = leb_mapping_get(ubi, vid->vol_id, vid->lnum, &incumbent);

		if (0 != ret) {
			LOG_WRN("PEB %u: claims volume %u block %u, which the "
				"volume table does not describe (%d); queued "
				"for reclaim",
				pnum, vid->vol_id, vid->lnum, ret);
			event_emit(ubi, UBI_EVENT_LEB_ORPHANED, pnum,
				   vid->vol_id, vid->lnum);
			peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
			continue;
		}

		if (UBI_LEB_UNMAPPED != incumbent) {
			struct ubi_headers other = { 0 };
			uint64_t incumbent_sqnum = 0;

			ret = ubi_headers_read(ubi, incumbent, &other);

			/* A block that will not say how old it is loses to
			 * one that will. */
			if (0 == ret && UBI_HEADER_OK == other.vid_status)
				incumbent_sqnum = other.vid.sqnum;

			const uint32_t stale = (incumbent_sqnum > vid->sqnum) ?
						       pnum :
						       incumbent;

			LOG_WRN("volume %u block %u is claimed by PEB %u and "
				"PEB %u; PEB %u is the older copy",
				vid->vol_id, vid->lnum, incumbent, pnum, stale);

			peb_state_set(ubi, stale, UBI_PEB_RECLAIM);

			if (stale == pnum)
				continue;
		}

		ret = leb_mapping_set(ubi, vid->vol_id, vid->lnum,
				      (uint16_t)pnum);

		if (0 != ret) {
			LOG_WRN("PEB %u: volume %u block %u went away between "
				"the two lookups (%d)",
				pnum, vid->vol_id, vid->lnum, ret);
			peb_state_set(ubi, pnum, UBI_PEB_RECLAIM);
		}
	}
}

/* Module interface function definitions ----------------------------------- */

size_t ubi_device_size(void)
{
	return sizeof(struct ubi_device);
}

int ubi_device_format(const struct ubi_config *config)
{
	int ret = config_validate(config);

	if (0 != ret) {
		LOG_ERR("format needs a key handle and both callbacks");
		return ret;
	}

	/*
	 * Formatting has no attached device, but it needs the same partition,
	 * geometry and keys, so it borrows a handle without the per-block
	 * bookkeeping, which it has no use for.
	 */
	struct ubi_device *ubi = k_malloc(sizeof(*ubi));

	if (NULL == ubi) {
		LOG_ERR("no memory for a device handle of %zu bytes",
			sizeof(*ubi));
		return -ENOMEM;
	}

	memset(ubi, 0, sizeof(*ubi));

	ret = device_open(ubi, config);

	if (0 != ret) {
		LOG_ERR("cannot open partition %u (%d): %u blocks of %u bytes, "
			"write granularity %u",
			config->flash_area_id, ret, ubi->geometry.peb_count,
			ubi->geometry.peb_size, ubi->geometry.write_block_size);
		k_free(ubi);
		return ret;
	}

	ret = image_seq_draw(&ubi->image_seq);

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

		ret = ubi_headers_read(ubi, pnum, &headers);

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
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		bool history_lost = false;

		if (UBI_VOLUME_TABLE_LEB_COUNT == written)
			break;

		ret = peb_prepare(ubi, pnum, &history_lost);

		if (0 != ret) {
			LOG_WRN("PEB %u cannot be prepared (%d), trying the "
				"next one",
				pnum, ret);
			continue;
		}

		if (history_lost)
			LOG_WRN("PEB %u: wear history was unreadable, the "
				"erase count restarts from zero",
				pnum);

		ubi->volume_table.eba[written] = (uint16_t)pnum;

		ret = ubi_volume_table_write(ubi, written, &record);

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

int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config)
{
	if (NULL == ubi) {
		LOG_ERR("attach needs a device handle");
		return -EINVAL;
	}

	if (UBI_DEVICE_MAGIC == ubi->magic) {
		LOG_ERR("this handle is already attached");
		return -EBUSY;
	}

	int ret = config_validate(config);

	if (0 != ret) {
		LOG_ERR("attach needs a key handle and both callbacks");
		return ret;
	}

	memset(ubi, 0, sizeof(*ubi));

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

	scan_first_pass(ubi, &unopenable);

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

		ret = ubi_volume_table_read(ubi, lnum, &record);

		if (0 != ret) {
			if (-EBADMSG == ret)
				event_emit(ubi, UBI_EVENT_VOLUME_TABLE_TAMPERED,
					   pnum, UBI_VOLUME_TABLE_VOL_ID, lnum);

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

	if (!volume_table_copies_agree(copy, ARRAY_SIZE(copy), &record)) {
		LOG_WRN("partition %u: not every copy of the volume table is "
			"current, so one erase would cost a revision",
			config->flash_area_id);
		event_emit(ubi, UBI_EVENT_VOLUME_TABLE_DEGRADED,
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

	ret = volumes_build(ubi, &record);

	if (0 != ret) {
		LOG_ERR("the volume table declares more logical blocks than "
			"the %u this partition has",
			ubi->geometry.peb_count);
		goto exit;
	}

	scan_second_pass(ubi);

	ret = state_check(ubi);

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

int ubi_device_deinit(struct ubi_device *ubi)
{
	if (NULL == ubi || UBI_DEVICE_MAGIC != ubi->magic) {
		LOG_ERR("this handle is not attached");
		return -EINVAL;
	}

	device_tables_free(ubi);
	device_close(ubi);

	memset(ubi, 0, sizeof(*ubi));

	return 0;
}

int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	if (NULL == ubi || UBI_DEVICE_MAGIC != ubi->magic || NULL == info) {
		LOG_ERR("device info needs an attached handle");
		return -EINVAL;
	}

	const int ret = device_info_fill(ubi, info);

	if (0 != ret) {
		LOG_ERR("the block state table is not one this build wrote");
		return ret;
	}

	return 0;
}
