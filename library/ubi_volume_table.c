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
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** "UBIV" - volume table record. */
#define UBI_VOLUME_TABLE_MAGIC (0x55424956UL)

/** One read fetches the VID header and the record that follows it. */
#define VOLUME_TABLE_READ_SIZE \
	(UBI_HEADER_SIZE + UBI_VOLUME_TABLE_RECORD_MAX_SIZE)

/**
 * Room for the record plus whatever padding the flash demands.
 *
 * Writes are rounded up to the write block size, and a device is only
 * attached when that size divides a header, so rounding up to one header is
 * always enough.
 */
#define VOLUME_TABLE_WRITE_SIZE \
	ROUND_UP(UBI_VOLUME_TABLE_RECORD_MAX_SIZE, UBI_HEADER_SIZE)

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

/** Bytes reserved for a name inside an entry. */
#define ENTRY_NAME_SIZE (UBI_VOLUME_NAME_MAX_LEN + 1)

BUILD_ASSERT(UBI_VOLUME_TABLE_PREAMBLE_SIZE == 32,
	     "the preamble layout is an on-flash contract");

BUILD_ASSERT(ENTRY_OFFSET_NAME + ENTRY_NAME_SIZE == UBI_VOLUME_TABLE_ENTRY_SIZE,
	     "the entry layout is an on-flash contract");

/* Static function declarations -------------------------------------------- */

/**
 * \brief Bytes a record with \p volume_count volumes occupies.
 */
static size_t volume_table_record_size(uint32_t volume_count);

/**
 * \brief Serialize and seal a record into \p buffer.
 */
static int
volume_table_record_serialize(const struct ubi_volume_table_record *record,
			      psa_key_id_t key_id, uint8_t *buffer,
			      size_t buffer_size, size_t *record_size);

/**
 * \brief Verify and decode a record, checking magic, then the MAC, then the
 *        entries, so that nothing read from the flash is used before it is
 *        authentic.
 */
static enum ubi_header_status
volume_table_record_parse(const uint8_t *buffer, size_t buffer_size,
			  psa_key_id_t key_id,
			  struct ubi_volume_table_record *record);

/* Static function definitions --------------------------------------------- */

static size_t volume_table_record_size(uint32_t volume_count)
{
	return UBI_VOLUME_TABLE_PREAMBLE_SIZE +
	       (size_t)volume_count * UBI_VOLUME_TABLE_ENTRY_SIZE +
	       UBI_MAC_SIZE;
}

static int
volume_table_record_serialize(const struct ubi_volume_table_record *record,
			      psa_key_id_t key_id, uint8_t *buffer,
			      size_t buffer_size, size_t *record_size)
{
	if (NULL == record || NULL == buffer || NULL == record_size ||
	    PSA_KEY_ID_NULL == key_id)
		return -EINVAL;

	if (CONFIG_UBI_MAX_NR_OF_VOLUMES < record->volume_count)
		return -EINVAL;

	const size_t needed = volume_table_record_size(record->volume_count);

	if (buffer_size < needed)
		return -EINVAL;

	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	size_t mac_offset = 0;
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

		sys_put_be32(entry->vol_id, &slot[ENTRY_OFFSET_VOL_ID]);
		sys_put_be32(entry->leb_count, &slot[ENTRY_OFFSET_LEB_COUNT]);
		/* Name is fixed width and already zeroed, so it is padded. */
		strncpy((char *)&slot[ENTRY_OFFSET_NAME], entry->name,
			ENTRY_NAME_SIZE - 1);
	}

	mac_offset = needed - UBI_MAC_SIZE;

	status = psa_mac_compute(key_id, PSA_ALG_CMAC, buffer, mac_offset,
				 &buffer[mac_offset], UBI_MAC_SIZE,
				 &mac_length);

	if (PSA_SUCCESS != status || UBI_MAC_SIZE != mac_length)
		return -EIO;

	*record_size = needed;

	return 0;
}

static enum ubi_header_status
volume_table_record_parse(const uint8_t *buffer, size_t buffer_size,
			  psa_key_id_t key_id,
			  struct ubi_volume_table_record *record)
{
	if (NULL == buffer || NULL == record || PSA_KEY_ID_NULL == key_id ||
	    UBI_VOLUME_TABLE_PREAMBLE_SIZE > buffer_size)
		return UBI_HEADER_ERROR;

	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	uint32_t volume_count = 0;
	size_t mac_offset = 0;
	size_t needed = 0;

	if (UBI_VOLUME_TABLE_MAGIC !=
	    sys_get_be32(&buffer[PREAMBLE_OFFSET_MAGIC])) {
		return UBI_HEADER_NOT_UBI;
	}

	/*
	 * The count sizes the authenticated span, so it has to be sane before
	 * it is used. A forged count could otherwise steer the MAC over
	 * memory past the buffer.
	 */
	volume_count = sys_get_be32(&buffer[PREAMBLE_OFFSET_VOLUME_COUNT]);

	if (CONFIG_UBI_MAX_NR_OF_VOLUMES < volume_count)
		return UBI_HEADER_NOT_UBI;

	needed = volume_table_record_size(volume_count);

	if (buffer_size < needed)
		return UBI_HEADER_CORRUPT;

	mac_offset = needed - UBI_MAC_SIZE;

	/* Constant-time comparison; never memcmp() a MAC. */
	status = psa_mac_verify(key_id, PSA_ALG_CMAC, buffer, mac_offset,
				&buffer[mac_offset], UBI_MAC_SIZE);

	if (PSA_ERROR_INVALID_SIGNATURE == status)
		return UBI_HEADER_TAMPERED;

	if (PSA_SUCCESS != status)
		return UBI_HEADER_ERROR;

	/* Only now is the version byte trustworthy. */
	if (UBI_VOLUME_TABLE_VERSION != buffer[PREAMBLE_OFFSET_VERSION])
		return UBI_HEADER_NOT_UBI;

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
		memcpy(entry->name, &slot[ENTRY_OFFSET_NAME],
		       ENTRY_NAME_SIZE - 1);
		entry->name[ENTRY_NAME_SIZE - 1] = '\0';
	}

	return UBI_HEADER_OK;
}

int ubi_impl_volume_table_read(const struct ubi_device *ubi, uint32_t lnum,
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

	uint8_t buffer[VOLUME_TABLE_READ_SIZE] = { 0 };
	const uint8_t *record_bytes = &buffer[UBI_HEADER_SIZE];
	int ret = ubi_impl_io_read(ubi, pnum, UBI_VID_HEADER_OFFSET, buffer,
				   sizeof(buffer));

	if (0 != ret) {
		LOG_ERR("PEB %u: volume table copy %u could not be read (%d)",
			pnum, lnum, ret);
		return ret;
	}

	struct ubi_vid_header vid = { 0 };
	const enum ubi_header_status status = ubi_impl_header_vid_parse(
		buffer, UBI_HEADER_SIZE, ubi->keys.header, pnum,
		ubi->geometry.erase_value, &vid);

	if (UBI_HEADER_OK != status)
		return -ENOENT;

	if (vid.data_size < UBI_VOLUME_TABLE_PREAMBLE_SIZE ||
	    vid.data_size > UBI_VOLUME_TABLE_RECORD_MAX_SIZE)
		return -ENOENT;

	/* The record was checksummed before it was stored, which is what tells
	 * a truncated write apart from a complete one. */
	if (vid.copy_flag &&
	    vid.data_crc != crc32_ieee(record_bytes, vid.data_size))
		return -ENOENT;

	const enum ubi_header_status record_status = volume_table_record_parse(
		record_bytes, vid.data_size, ubi->keys.volume_table, record);

	if (UBI_HEADER_TAMPERED == record_status) {
		LOG_ERR("PEB %u: volume table copy %u carries a MAC that does "
			"not match; it was modified",
			pnum, lnum);
		return -EBADMSG;
	}

	if (UBI_HEADER_OK != record_status) {
		LOG_ERR("PEB %u: volume table copy %u is not a record this "
			"build can read (%d)",
			pnum, lnum, record_status);
		return -ENOTSUP;
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

	uint8_t buffer[VOLUME_TABLE_WRITE_SIZE];
	size_t record_size = 0;

	/* Padding with the byte an erase leaves behind keeps the tail of the
	 * write block as if it had never been written; zeroes would burn it. */
	memset(buffer, ubi->geometry.erase_value, sizeof(buffer));

	int ret = volume_table_record_serialize(record, ubi->keys.volume_table,
						buffer, sizeof(buffer),
						&record_size);

	if (0 != ret) {
		LOG_ERR("PEB %u: volume table copy %u could not be sealed (%d)",
			pnum, lnum, ret);
		return ret;
	}

	const struct ubi_vid_header vid = {
		.sqnum = ubi->global_sqnum + 1,
		.vol_id = UBI_VOLUME_TABLE_VOL_ID,
		.lnum = lnum,
		.image_seq = ubi->image_seq,
		.data_size = (uint32_t)record_size,
		.data_crc = crc32_ieee(buffer, record_size),
		.copy_flag = true,
	};

	/* The VID header goes down first and already carries the length and
	 * the checksum, so an interrupted record is recognisable. */
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

	ubi->global_sqnum = vid.sqnum;
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

	const uint32_t spare =
		(ubi->volume_table.current + 1) % UBI_VOLUME_TABLE_LEB_COUNT;
	const uint32_t order[UBI_VOLUME_TABLE_LEB_COUNT] = {
		spare, ubi->volume_table.current
	};

	ubi->volume_table.degraded = false;

	for (uint32_t i = 0; i < UBI_VOLUME_TABLE_LEB_COUNT; ++i) {
		const uint32_t lnum = order[i];
		uint32_t pnum = ubi->volume_table.eba[lnum];
		int ret = 0;

		if (UBI_LEB_UNMAPPED == pnum) {
			/* A free block arrives erased and stamped already. */
			ret = ubi_impl_peb_allocate_for_write(ubi, &pnum);

			if (0 != ret) {
				LOG_ERR("no block to hold volume table copy "
					"%u (%d)",
					lnum, ret);
				return ret;
			}

			ubi->volume_table.eba[lnum] = (uint16_t)pnum;
		} else {
			/* Reusing the block this copy already sits in, so the
			 * old record has to go before the new one lands. */
			ret = ubi_impl_peb_prepare(ubi, pnum);

			if (0 != ret) {
				LOG_ERR("PEB %u cannot take volume table copy "
					"%u (%d)",
					pnum, lnum, ret);
				ubi->volume_table.eba[lnum] = UBI_LEB_UNMAPPED;
				return ret;
			}
		}

		ret = ubi_impl_volume_table_write(ubi, lnum, record);

		if (0 != ret) {
			LOG_ERR("PEB %u cannot hold volume table copy %u (%d)",
				pnum, lnum, ret);
			ubi->volume_table.eba[lnum] = UBI_LEB_UNMAPPED;
			return ret;
		}

		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_MAPPED);

		/* The freshly written copy is the one in force from here on,
		 * so an interruption of the second write loses nothing. */
		ubi->volume_table.current = lnum;
	}

	return 0;
}
