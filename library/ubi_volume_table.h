/**
 * \file    ubi_volume_table.h
 * \author  Kamil Kielbasa
 * \brief   The record that lists every volume on the device.
 *
 *          One record describes the whole device: its volumes and the
 *          geometry it was formatted for. It lives in an internal volume of
 *          two copies, each written into a fresh block on every update, and
 *          carries an AES-CMAC under a key of its own.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_VOLUME_TABLE_H
#define UBI_VOLUME_TABLE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_header.h"
#include "ubi_key.h"

/* Defines ----------------------------------------------------------------- */

/** Volume identifier reserved for the volume table itself. */
#define UBI_VOLUME_TABLE_VOL_ID (0xFFFFFFFEUL)

/** Copies of the volume table record. Both hold the same record except
 *  between the two writes of an update. */
#define UBI_VOLUME_TABLE_LEB_COUNT (2)

/** Name the internal volume answers to, for logs. */
#define UBI_VOLUME_TABLE_NAME "volume table"

/** Bytes preceding the first volume entry. */
#define UBI_VOLUME_TABLE_PREAMBLE_SIZE (32)

/** Bytes per volume entry. */
#define UBI_VOLUME_TABLE_ENTRY_SIZE (24)

/** Record format understood by this implementation. */
#define UBI_VOLUME_TABLE_VERSION (1)

/** Largest record this build can produce or accept. */
#define UBI_VOLUME_TABLE_RECORD_MAX_SIZE                              \
	(UBI_VOLUME_TABLE_PREAMBLE_SIZE +                             \
	 CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_VOLUME_TABLE_ENTRY_SIZE + \
	 UBI_MAC_SIZE)

/** Largest data area a copy can claim: writes, and relocation's seal, are
 *  rounded up to a write block, which divides a header. */
#define UBI_VOLUME_TABLE_DATA_MAX_SIZE \
	ROUND_UP(UBI_VOLUME_TABLE_RECORD_MAX_SIZE, UBI_HEADER_SIZE)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief One volume, as recorded on the flash.
 */
struct ubi_volume_table_entry {
	/** Identifier assigned at creation, never reused. */
	uint32_t vol_id;
	/** Logical erase blocks reserved for this volume. */
	uint32_t leb_count;
	/** NUL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
};

/**
 * \brief The whole record, in decoded form.
 */
struct ubi_volume_table_record {
	/** Incremented on every change. */
	uint32_t revision;
	/** Image the device is. Leftover blocks carry an earlier one, and only
	 *  a format writes this record. */
	uint32_t image_seq;
	/** Block size the device was formatted for. */
	uint32_t peb_size;
	/** Blocks the device was formatted for. */
	uint32_t peb_count;
	/** Next volume identifier. */
	uint32_t vol_id_watermark;
	/** Entries in use. */
	uint32_t volume_count;
	/** The volumes themselves. */
	struct ubi_volume_table_entry entries[CONFIG_UBI_MAX_NR_OF_VOLUMES];
};

struct ubi_device;

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Serialize and seal a record.
 *
 * \param[in] record                    Record to write out.
 * \param key_id                        Volume table key.
 * \param[out] buffer                   Receives the record.
 * \param buffer_size                   Bytes available in \p buffer.
 * \param[out] record_size              Bytes of \p buffer the record took.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         An argument is missing, the record lists more volumes than this
 *         build allows, or it does not fit \p buffer.
 * \retval -EIO
 *         The crypto backend failed.
 */
int ubi_impl_volume_table_record_serialize(
	const struct ubi_volume_table_record *record, psa_key_id_t key_id,
	uint8_t *buffer, size_t buffer_size, size_t *record_size);

/**
 * \brief Verify and decode a record: magic, volume count, MAC, version, and
 *        only then the fields.
 *
 * \param[in] buffer                    Bytes to decode; the record ends where
 *                                      its own volume count says.
 * \param buffer_size                   Bytes available in \p buffer.
 * \param key_id                        Volume table key.
 * \param[out] record                   Receives the record; untouched unless
 *                                      the result is #UBI_HEADER_OK.
 *
 * \return #UBI_HEADER_OK, #UBI_HEADER_NOT_UBI for a wrong magic,
 *         #UBI_HEADER_CORRUPT when the record would not fit \p buffer,
 *         #UBI_HEADER_TAMPERED when the MAC fails, #UBI_HEADER_UNSUPPORTED
 *         for another version or more volumes than this build allows, or
 *         #UBI_HEADER_ERROR for a missing argument or a crypto failure.
 */
enum ubi_header_status
ubi_impl_volume_table_record_parse(const uint8_t *buffer, size_t buffer_size,
				   psa_key_id_t key_id,
				   struct ubi_volume_table_record *record);

/**
 * \brief Read and verify one copy of the volume table record.
 *
 *        The seal in its header only says the copy was written in full;
 *        relocation may leave it short of the record or past it. The record's
 *        own volume count says where it ends.
 *
 * \param[in,out] ubi                   Device holding the partition; its
 *                                      scratch buffer is used.
 * \param lnum                          Which copy to read.
 * \param[out] record                   Decoded record, written only on
 *                                      success.
 *
 * \retval 0
 *         The record is authentic and complete.
 * \retval -EINVAL
 *         A pointer is \c NULL, or \p lnum names no copy.
 * \retval -ENOENT
 *         No block holds that copy, or the one that does holds nothing
 *         finished.
 * \retval -EBADMSG
 *         The copy is complete but its MAC does not verify, or its record
 *         names another image than its header.
 * \retval -ENOTSUP
 *         Authentic, but another version or more volumes than this build
 *         allows.
 * \retval -EIO
 *         The flash driver or the crypto backend failed.
 */
int ubi_impl_volume_table_read(struct ubi_device *ubi, uint32_t lnum,
			       struct ubi_volume_table_record *record);

/**
 * \brief Write one copy of the volume table record into the block recorded
 *        for \p lnum, which must be erased and stamped.
 *
 *        The VID header goes down first and carries the length and the
 *        checksum, so an interrupted copy reads as \c -ENOENT.
 *
 * \param[in,out] ubi                   Device holding the partition.
 * \param lnum                          Which copy to write.
 * \param[in] record                    Record to write.
 *
 * \retval 0
 *         Written.
 * \retval -EINVAL
 *         A pointer is \c NULL, \p lnum names no copy, or the record is
 *         malformed.
 * \retval -ENOENT
 *         No block is recorded for that copy.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_volume_table_write(struct ubi_device *ubi, uint32_t lnum,
				const struct ubi_volume_table_record *record);

/**
 * \brief Put a record in force, leaving both copies carrying it.
 *
 *        Each copy goes into a fresh block, the one not in force first, and
 *        the block it leaves is erased at once. The record is in force once
 *        the first copy is down; if the second cannot be written, the table
 *        is left degraded and says so.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[in] record                    Record to put in force.
 *
 * \retval 0
 *         The record is in force.
 * \retval -EINVAL
 *         A pointer is \c NULL, or the record is malformed.
 * \retval -ENOSPC
 *         No block for the first copy; what was in force still is.
 * \retval -EIO
 *         The crypto backend or the flash driver failed before the first
 *         copy was down; what was in force still is.
 */
int ubi_impl_volume_table_commit(struct ubi_device *ubi,
				 const struct ubi_volume_table_record *record);

#endif /* UBI_VOLUME_TABLE_H */
