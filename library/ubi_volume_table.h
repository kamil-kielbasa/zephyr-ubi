/**
 * \file    ubi_volume_table.h
 * \author  Kamil Kielbasa
 * \brief   The record that lists every volume on the device.
 *
 *          One record describes the whole device: which volumes exist, how
 *          many logical blocks each was given, and the geometry the device
 *          was formatted for. It lives in an internal volume of its own, so
 *          it wanders with wear levelling instead of pinning two blocks.
 *
 *          UBI stores application data verbatim, which means the record
 *          cannot ride on that path: it carries its own AES-CMAC tag, under a
 *          key separate from the one guarding block headers. The tag of the
 *          VID header proves which block the record sits in; this tag proves
 *          the record's contents.
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

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_header.h"

/* Defines ----------------------------------------------------------------- */

/**
 * Volume identifier reserved for the volume table itself.
 *
 * Linux UBI calls this the layout volume; the name here says what it holds.
 */
#define UBI_VOLUME_TABLE_VOL_ID (0xFFFFFFFEUL)

/**
 * Logical blocks holding the volume table record.
 *
 * Both always hold the same record. Keeping one of them a revision behind
 * would mean that erasing the newer block silently takes the device back to
 * the older layout, so an update writes the spare first and then the other,
 * and only the window between those two writes has them disagreeing.
 */
#define UBI_VOLUME_TABLE_LEB_COUNT (2)

/** Name the internal volume answers to, for logs and introspection. */
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
	 UBI_HEADER_TAG_SIZE)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief One volume, as recorded on the flash.
 */
struct ubi_volume_table_entry {
	/** Identifier assigned at creation, never reused. */
	uint32_t vol_id;
	/** Logical erase blocks reserved for this volume. */
	uint32_t leb_count;
	/** NULL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
};

/**
 * \brief The whole record, in decoded form.
 */
struct ubi_volume_table_record {
	/** Incremented on every change; reported as
	 *  \ref ubi_device_info.revision. */
	uint32_t revision;
	/**
	 * Image this record belongs to, and the authority on the matter.
	 *
	 * Block headers carry the same value, but a leftover block from an
	 * earlier format carries a valid one too. Only the record settles
	 * which image the device actually is, because only ubi_device_format()
	 * writes it.
	 */
	uint32_t image_seq;
	/** Geometry the device was formatted for. Attach refuses a partition
	 *  that no longer matches, rather than misreading it. */
	uint32_t peb_size;
	/** Blocks the device was formatted for. */
	uint32_t peb_count;
	/** Highest volume identifier ever handed out. */
	uint32_t vol_id_watermark;
	/** Entries in use. */
	uint32_t volume_count;
	/** The volumes themselves. */
	struct ubi_volume_table_entry entries[CONFIG_UBI_MAX_NR_OF_VOLUMES];
};

struct ubi_device;

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Read and verify one copy of the volume table record.
 *
 *        One flash read fetches the VID header and the record behind it. The
 *        header carries the record's length and checksum, and the header is
 *        sealed, so a changed record is caught by a checksum its author could
 *        not have repaired without the key.
 *
 * \param[in] ubi                       Device holding the partition. The
 *                                      block to read is the one recorded for
 *                                      \p lnum.
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
 *         finished; look for the other copy.
 * \retval -EBADMSG
 *         The bytes are complete but the tag does not verify.
 * \retval -ENOTSUP
 *         The record is authentic but this build cannot read its version.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_volume_table_read(const struct ubi_device *ubi, uint32_t lnum,
			  struct ubi_volume_table_record *record);

/**
 * \brief Write one copy of the volume table record.
 *
 *        Goes to the block recorded for \p lnum, which must already carry an
 *        erase counter header and nothing else, and takes the next sequence
 *        number from the device. The VID header goes down before the record,
 *        so an interruption leaves something \ref ubi_volume_table_read
 *        reports as \c -ENOENT.
 *
 * \param[in,out] ubi                   Device holding the partition; its
 *                                      sequence number is advanced.
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
int ubi_volume_table_write(struct ubi_device *ubi, uint32_t lnum,
			   const struct ubi_volume_table_record *record);

/**
 * \brief Put a record in force, leaving both copies carrying it.
 *
 *        Writes the copy that is not in force, switches to it, then writes
 *        the other one. An interruption therefore always leaves at least one
 *        complete copy, and the one with the higher sequence number is the
 *        newer of the two. A pair that was already out of step is brought
 *        back into line on the way through.
 *
 *        The device is left describing whatever actually reached the flash:
 *        on success both copies carry \p record, and on failure the caller's
 *        own state is the only thing that still needs rolling back.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[in] record                    Record to put in force.
 *
 * \retval 0
 *         Both copies carry it.
 * \retval -EINVAL
 *         A pointer is \c NULL, or the record is malformed.
 * \retval -ENOSPC
 *         No block could be found to hold a copy.
 * \retval -EIO
 *         The crypto backend or the flash driver failed before any copy was
 *         written; what was in force still is.
 */
int ubi_volume_table_commit(struct ubi_device *ubi,
			    const struct ubi_volume_table_record *record);

#endif /* UBI_VOLUME_TABLE_H */
