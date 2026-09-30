/**
 * \file    ubi_volume_table.c
 * \author  Kamil Kielbasa
 * \brief   The record that lists every volume on the device.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_io.h"
#include "ubi_key.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_state.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** "UBIV" - volume table record. */
#define UBI_VOLUME_TABLE_MAGIC (0x55424956UL)

/** Fresh blocks a copy is tried in before the commit gives up on it. */
#define VOLUME_TABLE_ATTEMPTS (2)

/* Preamble field offsets. */
#define PREAMBLE_OFFSET_MAGIC (0x00)
#define PREAMBLE_OFFSET_VERSION (0x04)
#define PREAMBLE_OFFSET_REVISION (0x08)
#define PREAMBLE_OFFSET_IMAGE_SEQ (0x0C)
#define PREAMBLE_OFFSET_PEB_SIZE (0x10)
#define PREAMBLE_OFFSET_PEB_COUNT (0x14)
#define PREAMBLE_OFFSET_VOL_ID_WATERMARK (0x18)
#define PREAMBLE_OFFSET_VOLUME_COUNT (0x1C)

/* Entry field offsets, relative to the entry. */
#define ENTRY_OFFSET_VOL_ID (0x00)
#define ENTRY_OFFSET_LEB_COUNT (0x04)
#define ENTRY_OFFSET_NAME (0x08)

/** Bytes of a name on the flash, NUL-padded; a name that fills them has no
 *  terminator. */
#define ENTRY_NAME_SIZE (16)

BUILD_ASSERT(UBI_VOLUME_TABLE_PREAMBLE_SIZE == 32,
	     "the preamble layout is an on-flash contract");

BUILD_ASSERT(ENTRY_OFFSET_NAME + ENTRY_NAME_SIZE == UBI_VOLUME_TABLE_ENTRY_SIZE,
	     "the entry layout is an on-flash contract");

BUILD_ASSERT(ENTRY_NAME_SIZE == UBI_VOLUME_NAME_MAX_LEN,
	     "the longest name fills the name field");

/* Static function declarations -------------------------------------------- */

/**
 * \brief Bytes a record with \p volume_count volumes occupies.
 */
static size_t volume_table_record_size(uint32_t volume_count);

/**
 * \brief Write one copy into a fresh block, switch to it and erase the block
 *        it replaces. A block that fails is retired, and the copy stays
 *        where it was.
 */
static int volume_table_copy_put(struct ubi_device *ubi, uint32_t lnum,
				 const struct ubi_volume_table_record *record);

/* Static function definitions --------------------------------------------- */

static size_t volume_table_record_size(uint32_t volume_count)
{
	return UBI_VOLUME_TABLE_PREAMBLE_SIZE +
	       (size_t)volume_count * UBI_VOLUME_TABLE_ENTRY_SIZE +
	       UBI_MAC_SIZE;
}

static int volume_table_copy_put(struct ubi_device *ubi, uint32_t lnum,
				 const struct ubi_volume_table_record *record)
{
	const uint16_t replaced = ubi->volume_table.eba[lnum];
	int ret = -EIO;

	for (uint32_t attempt = 0; attempt < VOLUME_TABLE_ATTEMPTS; ++attempt) {
		uint32_t pnum = 0;

		ret = ubi_impl_peb_allocate_for_write(ubi, &pnum);

		if (-ENOSPC == ret || ubi->read_only)
			return ret;

		if (0 != ret)
			continue;

		ubi->volume_table.eba[lnum] = (uint16_t)pnum;

		ret = ubi_impl_volume_table_write(ubi, lnum, record);

		if (0 != ret) {
			LOG_ERR("PEB %u cannot hold volume table copy %u (%d)",
				pnum, lnum, ret);
			ubi->volume_table.eba[lnum] = replaced;
			ubi_impl_peb_retire(ubi, pnum, UBI_VOLUME_TABLE_VOL_ID,
					    lnum);
			continue;
		}

		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

		if (UBI_LEB_UNMAPPED == replaced)
			return 0;

		/* Left for reclaim, an older table could stand in for lost
		 * copies. The switch stands either way. */
		ret = ubi_impl_peb_prepare(ubi, replaced);

		if (0 != ret)
			ubi_impl_peb_retire(ubi, replaced, UBI_VOL_ID_INVALID,
					    0);

		return 0;
	}

	return ret;
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_volume_table_record_serialize(
	const struct ubi_volume_table_record *record, psa_key_id_t key_id,
	uint8_t *buffer, size_t buffer_size, size_t *record_size)
{
	if (NULL == record || NULL == buffer || NULL == record_size ||
	    PSA_KEY_ID_NULL == key_id)
		return -EINVAL;

	if (CONFIG_UBI_MAX_NR_OF_VOLUMES < record->volume_count)
		return -EINVAL;

	const size_t needed = volume_table_record_size(record->volume_count);

	if (buffer_size < needed)
		return -EINVAL;

	const size_t mac_offset = needed - UBI_MAC_SIZE;
	size_t mac_length = 0;

	memset(buffer, 0, needed);

	sys_put_be32(UBI_VOLUME_TABLE_MAGIC, &buffer[PREAMBLE_OFFSET_MAGIC]);
	buffer[PREAMBLE_OFFSET_VERSION] = UBI_VOLUME_TABLE_VERSION;
	sys_put_be32(record->revision, &buffer[PREAMBLE_OFFSET_REVISION]);
	sys_put_be32(record->image_seq, &buffer[PREAMBLE_OFFSET_IMAGE_SEQ]);
	sys_put_be32(record->peb_size, &buffer[PREAMBLE_OFFSET_PEB_SIZE]);
	sys_put_be32(record->peb_count, &buffer[PREAMBLE_OFFSET_PEB_COUNT]);
	sys_put_be32(record->vol_id_watermark,
		     &buffer[PREAMBLE_OFFSET_VOL_ID_WATERMARK]);
	sys_put_be32(record->volume_count,
		     &buffer[PREAMBLE_OFFSET_VOLUME_COUNT]);

	for (uint32_t i = 0; i < record->volume_count; ++i) {
		const struct ubi_volume_table_entry *entry =
			&record->entries[i];
		uint8_t *slot =
			&buffer[UBI_VOLUME_TABLE_PREAMBLE_SIZE +
				(size_t)i * UBI_VOLUME_TABLE_ENTRY_SIZE];
		const char *end = memchr(entry->name, '\0', ENTRY_NAME_SIZE);
		const size_t name_length = (NULL == end) ?
						   ENTRY_NAME_SIZE :
						   (size_t)(end - entry->name);

		sys_put_be32(entry->vol_id, &slot[ENTRY_OFFSET_VOL_ID]);
		sys_put_be32(entry->leb_count, &slot[ENTRY_OFFSET_LEB_COUNT]);
		memcpy(&slot[ENTRY_OFFSET_NAME], entry->name, name_length);
	}

	const psa_status_t status =
		psa_mac_compute(key_id, PSA_ALG_CMAC, buffer, mac_offset,
				&buffer[mac_offset], UBI_MAC_SIZE, &mac_length);

	if (PSA_SUCCESS != status || UBI_MAC_SIZE != mac_length)
		return -EIO;

	*record_size = needed;

	return 0;
}

enum ubi_header_status
ubi_impl_volume_table_record_parse(const uint8_t *buffer, size_t buffer_size,
				   psa_key_id_t key_id,
				   struct ubi_volume_table_record *record)
{
	if (NULL == buffer || NULL == record || PSA_KEY_ID_NULL == key_id ||
	    UBI_VOLUME_TABLE_PREAMBLE_SIZE > buffer_size)
		return UBI_HEADER_ERROR;

	const uint32_t magic = sys_get_be32(&buffer[PREAMBLE_OFFSET_MAGIC]);

	if (UBI_VOLUME_TABLE_MAGIC != magic)
		return UBI_HEADER_NOT_UBI;

	/* The count sizes the authenticated span, so a forged one must not
	 * steer the MAC past the buffer. */
	const uint32_t volume_count =
		sys_get_be32(&buffer[PREAMBLE_OFFSET_VOLUME_COUNT]);

	if (CONFIG_UBI_MAX_NR_OF_VOLUMES < volume_count)
		return UBI_HEADER_UNSUPPORTED;

	const size_t needed = volume_table_record_size(volume_count);

	if (buffer_size < needed)
		return UBI_HEADER_CORRUPT;

	const size_t mac_offset = needed - UBI_MAC_SIZE;

	/* Constant-time comparison; never memcmp() a MAC. */
	const psa_status_t status =
		psa_mac_verify(key_id, PSA_ALG_CMAC, buffer, mac_offset,
			       &buffer[mac_offset], UBI_MAC_SIZE);

	if (PSA_ERROR_INVALID_SIGNATURE == status)
		return UBI_HEADER_TAMPERED;

	if (PSA_SUCCESS != status)
		return UBI_HEADER_ERROR;

	/* Only now is the version byte trustworthy. */
	if (UBI_VOLUME_TABLE_VERSION != buffer[PREAMBLE_OFFSET_VERSION])
		return UBI_HEADER_UNSUPPORTED;

	memset(record, 0, sizeof(*record));

	record->revision = sys_get_be32(&buffer[PREAMBLE_OFFSET_REVISION]);
	record->image_seq = sys_get_be32(&buffer[PREAMBLE_OFFSET_IMAGE_SEQ]);
	record->peb_size = sys_get_be32(&buffer[PREAMBLE_OFFSET_PEB_SIZE]);
	record->peb_count = sys_get_be32(&buffer[PREAMBLE_OFFSET_PEB_COUNT]);
	record->vol_id_watermark =
		sys_get_be32(&buffer[PREAMBLE_OFFSET_VOL_ID_WATERMARK]);
	record->volume_count = volume_count;

	for (uint32_t i = 0; i < volume_count; ++i) {
		struct ubi_volume_table_entry *entry = &record->entries[i];
		const uint8_t *slot =
			&buffer[UBI_VOLUME_TABLE_PREAMBLE_SIZE +
				(size_t)i * UBI_VOLUME_TABLE_ENTRY_SIZE];

		entry->vol_id = sys_get_be32(&slot[ENTRY_OFFSET_VOL_ID]);
		entry->leb_count = sys_get_be32(&slot[ENTRY_OFFSET_LEB_COUNT]);
		memcpy(entry->name, &slot[ENTRY_OFFSET_NAME], ENTRY_NAME_SIZE);
		entry->name[ENTRY_NAME_SIZE] = '\0';
	}

	return UBI_HEADER_OK;
}

int ubi_impl_volume_table_read(struct ubi_device *ubi, uint32_t lnum,
			       struct ubi_volume_table_record *record)
{
	if (NULL == ubi || NULL == record ||
	    lnum >= UBI_VOLUME_TABLE_LEB_COUNT) {
		LOG_ERR("reading volume table copy %u got bad arguments", lnum);
		return -EINVAL;
	}

	const uint32_t pnum = ubi->volume_table.eba[lnum];

	if (UBI_LEB_UNMAPPED == pnum)
		return -ENOENT;

	uint8_t *buffer = ubi->scratch->io;
	const uint8_t *data = &buffer[UBI_HEADER_SIZE];
	struct ubi_vid_header vid = { 0 };
	int ret = ubi_impl_io_read(ubi, pnum, UBI_VID_HEADER_OFFSET, buffer,
				   sizeof(ubi->scratch->io));

	if (0 != ret) {
		LOG_ERR("PEB %u: volume table copy %u could not be read (%d)",
			pnum, lnum, ret);
		return ret;
	}

	const enum ubi_header_status status = ubi_impl_header_vid_parse(
		buffer, UBI_HEADER_SIZE, ubi->keys.header, pnum,
		ubi->geometry.erase_value, &vid);

	if (UBI_HEADER_ERROR == status)
		return -EIO;

	if (UBI_HEADER_OK != status)
		return -ENOENT;

	/* Every copy is written sealed, so one without a seal was cut short
	 * before its data went down. */
	if (!vid.copy_flag || vid.data_size < UBI_VOLUME_TABLE_PREAMBLE_SIZE)
		return -ENOENT;

	/* A bit error in the erased tail takes a relocated seal this far. */
	if (vid.data_size > UBI_VOLUME_TABLE_DATA_MAX_SIZE) {
		ret = ubi_impl_header_vid_data_verify(ubi, pnum, &vid);

		if (-EBADMSG == ret)
			return -ENOENT;

		if (0 != ret)
			return ret;
	} else {
		const uint32_t crc = crc32_ieee(data, vid.data_size);

		if (vid.data_crc != crc)
			return -ENOENT;
	}

	/* Compared before decoding, so that a copy refused here leaves the
	 * record untouched. */
	const uint32_t image_seq =
		sys_get_be32(&data[PREAMBLE_OFFSET_IMAGE_SEQ]);

	if (vid.image_seq != image_seq) {
		LOG_ERR("PEB %u: volume table copy %u sits under a header of "
			"image 0x%08x but declares image 0x%08x",
			pnum, lnum, vid.image_seq, image_seq);
		return -EBADMSG;
	}

	const enum ubi_header_status record_status =
		ubi_impl_volume_table_record_parse(
			data, UBI_VOLUME_TABLE_DATA_MAX_SIZE,
			ubi->keys.volume_table, record);

	if (UBI_HEADER_UNSUPPORTED == record_status) {
		LOG_ERR("PEB %u: volume table copy %u was written by a build "
			"this one cannot read",
			pnum, lnum);
		return -ENOTSUP;
	}

	if (UBI_HEADER_ERROR == record_status) {
		LOG_ERR("PEB %u: volume table copy %u could not be checked",
			pnum, lnum);
		return -EIO;
	}

	if (UBI_HEADER_OK != record_status) {
		LOG_ERR("PEB %u: volume table copy %u does not verify (%d); it "
			"was modified",
			pnum, lnum, record_status);
		return -EBADMSG;
	}

	return 0;
}

int ubi_impl_volume_table_write(struct ubi_device *ubi, uint32_t lnum,
				const struct ubi_volume_table_record *record)
{
	if (NULL == ubi || NULL == record ||
	    lnum >= UBI_VOLUME_TABLE_LEB_COUNT) {
		LOG_ERR("writing volume table copy %u got bad arguments", lnum);
		return -EINVAL;
	}

	const uint32_t pnum = ubi->volume_table.eba[lnum];

	if (UBI_LEB_UNMAPPED == pnum) {
		LOG_ERR("no block is set aside for volume table copy %u", lnum);
		return -ENOENT;
	}

	uint8_t *buffer = &ubi->scratch->io[UBI_HEADER_SIZE];
	size_t record_size = 0;

	/* The erase value pads the tail of the last write block without
	 * programming it. */
	memset(buffer, ubi->geometry.erase_value,
	       UBI_VOLUME_TABLE_DATA_MAX_SIZE);

	int ret = ubi_impl_volume_table_record_serialize(
		record, ubi->keys.volume_table, buffer,
		UBI_VOLUME_TABLE_DATA_MAX_SIZE, &record_size);

	if (0 != ret) {
		LOG_ERR("PEB %u: volume table copy %u could not be sealed (%d)",
			pnum, lnum, ret);
		return ret;
	}

	const struct ubi_vid_header vid = {
		.sqnum = ubi_impl_sqnum_next(ubi),
		.vol_id = UBI_VOLUME_TABLE_VOL_ID,
		.lnum = lnum,
		.image_seq = ubi->image_seq,
		.data_size = (uint32_t)record_size,
		.data_crc = crc32_ieee(buffer, record_size),
		.copy_flag = true,
	};

	ret = ubi_impl_header_vid_write(ubi, pnum, &vid);

	if (0 != ret)
		return ret;

	ret = ubi_impl_io_write_data(ubi, pnum, 0, buffer,
				     ROUND_UP(record_size,
					      ubi->geometry.write_block_size));

	if (0 != ret) {
		LOG_ERR("PEB %u: volume table copy %u could not be written "
			"(%d)",
			pnum, lnum, ret);
		return ret;
	}

	ubi->volume_table.sqnum[lnum] = vid.sqnum;

	return 0;
}

int ubi_impl_volume_table_commit(struct ubi_device *ubi,
				 const struct ubi_volume_table_record *record)
{
	if (NULL == ubi || NULL == record) {
		LOG_ERR("committing the volume table got bad arguments");
		return -EINVAL;
	}

	const uint32_t previous = ubi->volume_table.current;
	const uint32_t spare = (previous + 1) % UBI_VOLUME_TABLE_LEB_COUNT;
	int ret = volume_table_copy_put(ubi, spare, record);

	/* Both copies are where they were, so what was in force still is. */
	if (0 != ret)
		return ret;

	/* The next attach finds this record, whatever becomes of the second
	 * copy. */
	ubi->volume_table.current = spare;

	/* Erasing the block the first copy replaced may have failed. */
	ret = ubi->read_only ? -EROFS :
			       volume_table_copy_put(ubi, previous, record);

	if (0 == ret) {
		ubi->volume_table.degraded = false;
		return 0;
	}

	LOG_ERR("the volume table is down to one current copy (%d)", ret);

	ubi->volume_table.degraded = true;
	ubi_impl_event_emit(ubi, UBI_EVENT_VOLUME_TABLE_DEGRADED,
			    ubi->volume_table.eba[spare],
			    UBI_VOLUME_TABLE_VOL_ID, spare);

	const uint16_t stale = ubi->volume_table.eba[previous];

	if (UBI_LEB_UNMAPPED == stale)
		return 0;

	/* Left mapped, relocation would reseal the old record newer than the
	 * one in force. */
	ubi->volume_table.eba[previous] = UBI_LEB_UNMAPPED;
	ubi_impl_peb_state_set(ubi, stale, UBI_PEB_RECLAIM);

	if (ubi->read_only)
		return 0;

	ret = ubi_impl_peb_reclaim(ubi, stale);

	if (0 != ret) {
		LOG_ERR("PEB %u: the replaced volume table copy could not be "
			"erased (%d) and stays on the flash",
			stale, ret);
	}

	return 0;
}
