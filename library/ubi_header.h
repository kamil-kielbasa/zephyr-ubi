/**
 * \file    ubi_header.h
 * \author  Kamil Kielbasa
 * \brief   On-flash header serialization and authentication.
 *
 *          Every physical erase block opens with two 64-byte headers: the
 *          erase counter (EC) header at offset zero and the volume identifier
 *          (VID) header right behind it, so that one 128-byte read pulls both.
 *
 *          Both carry a CRC32 and an AES-CMAC. The CRC tells an interrupted
 *          write apart from a deliberate change; only the MAC decides
 *          authenticity.
 *
 *          The MAC covers the physical block number followed by every field
 *          except the MAC and the CRC, which is what stops a header from
 *          being copied to another block. The magic is inside that input, so
 *          an EC MAC can never pass for a VID one.
 *
 *          The erase counter header stays byte-compatible with Linux UBI:
 *          its MAC sits in \c padding2. The volume identifier header does
 *          not, because Linux leaves no 16-byte hole in it. There the MAC
 *          takes the space of \c used_ebs, \c data_pad and \c data_crc,
 *          none of which a dynamic-only implementation has any use for, and
 *          this implementation's own data checksum moves to \c padding3.
 *
 *          Changing what goes into that input changes the on-flash format,
 *          so such a change travels as a bump of #UBI_HEADER_VERSION.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_HEADER_H
#define UBI_HEADER_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PSA headers: */
#include <psa/crypto.h>

/* Defines ----------------------------------------------------------------- */

/** Size of both the EC and the VID header, matching Linux UBI. */
#define UBI_HEADER_SIZE (64)

/** Offset of the EC header within a physical erase block. */
#define UBI_EC_HEADER_OFFSET (0)

/** Offset of the VID header within a physical erase block. */
#define UBI_VID_HEADER_OFFSET (64)

/** Offset at which block data begins. */
#define UBI_DATA_OFFSET (128)

/**
 * Header format understood by this implementation.
 *
 * Bump this for any change to the field layout or to what the MAC covers.
 * The version byte is checked after the MAC verifies, so an image written by
 * a different format reports #UBI_HEADER_NOT_UBI rather than looking like
 * tampering.
 */
#define UBI_HEADER_VERSION (1)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief Verdict of parsing a header off the flash.
 */
enum ubi_header_status {
	/** Magic, CRC and MAC all check out; the fields are trustworthy. */
	UBI_HEADER_OK = 0,
	/** Every byte reads as the flash's erased value; the block carries no
	 *  header. */
	UBI_HEADER_ERASED,
	/** Magic does not match, the format version is unknown, or the block
	 *  layout is not the one this build uses. */
	UBI_HEADER_NOT_UBI,
	/** CRC mismatch: an interrupted write or bit rot. */
	UBI_HEADER_CORRUPT,
	/** CRC matches but the MAC does not: the header was modified. */
	UBI_HEADER_TAMPERED,
	/** The crypto backend failed; the header says nothing either way. */
	UBI_HEADER_ERROR,
};

/**
 * \brief Erase counter header, in decoded form.
 *
 *        The CRC and the MAC are deliberately absent. They are how the bytes
 *        are checked, not information the caller acts on, and exposing the
 *        MAC would invite someone to compare it by hand.
 */
struct ubi_ec_header {
	/** Times this physical block has been erased. */
	uint64_t erase_count;
	/** Image this block belongs to; a mismatch makes it foreign. */
	uint32_t image_seq;
	/** Offset of the VID header, always #UBI_VID_HEADER_OFFSET. */
	uint32_t vid_header_offset;
	/** Offset of the data area, always #UBI_DATA_OFFSET. */
	uint32_t data_offset;
};

/**
 * \brief Volume identifier header, in decoded form.
 */
struct ubi_vid_header {
	/** Global sequence number; the highest wins when two blocks claim
	 *  the same logical block. */
	uint64_t sqnum;
	/** Volume this logical block belongs to. */
	uint32_t vol_id;
	/** Logical block number within the volume. */
	uint32_t lnum;
	/** Image this block belongs to. */
	uint32_t image_seq;
	/** Bytes of data written, meaningful only when \p copy_flag is set. */
	uint32_t data_size;
	/** CRC32 over those bytes, meaningful only when \p copy_flag is set. */
	uint32_t data_crc;
	/** The data area was written in full before this header was sealed,
	 *  so \p data_size and \p data_crc can tell whether a power loss
	 *  truncated it. Set by ubi_leb_change() and by relocation. */
	bool copy_flag;
};

/**
 * \brief Both headers of one physical erase block, read in one go.
 *
 *        They are adjacent, so fetching them together costs one flash read
 *        rather than two. The verdicts travel with the fields because a scan
 *        has to act on them separately: an erase counter header can verify
 *        while the volume identifier header behind it is blank.
 */
struct ubi_headers {
	/** Verdict of parsing the erase counter header. */
	enum ubi_header_status ec_status;
	/** Erase counter header, filled only on #UBI_HEADER_OK. */
	struct ubi_ec_header ec;
	/** Verdict of parsing the volume identifier header. */
	enum ubi_header_status vid_status;
	/** Volume identifier header, filled only on #UBI_HEADER_OK. */
	struct ubi_vid_header vid;
};

struct ubi_device;

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Serialize and seal an erase counter header.
 *
 *        Lays the fields out big-endian, computes the MAC over \p pnum and
 *        the fields, then computes the CRC over everything including the MAC.
 *
 * \param[in] header                    Fields to encode.
 * \param key_id                        CMAC key.
 * \param pnum                          Physical block the header will live
 *                                      in; bound into the MAC.
 * \param[out] buffer                   Receives #UBI_HEADER_SIZE bytes.
 * \param buffer_size                   Bytes available at \p buffer.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         A pointer is \c NULL, \p key_id is \c PSA_KEY_ID_NULL, or
 *         \p buffer is too small.
 * \retval -EIO
 *         The crypto backend failed.
 */
int ubi_impl_header_ec_serialize(const struct ubi_ec_header *header,
				 psa_key_id_t key_id, uint32_t pnum,
				 uint8_t *buffer, size_t buffer_size);

/**
 * \brief Verify and decode an erase counter header.
 *
 *        Checks magic, then CRC, then MAC, and only then touches the fields.
 *        Nothing read from the flash is used before the MAC verifies, so a
 *        forged header cannot steer where UBI reads next. Once the MAC
 *        verifies, the recorded block layout is checked against the one this
 *        build uses; an authentic header describing a different layout is
 *        reported as #UBI_HEADER_NOT_UBI.
 *
 * \param[in] buffer                    Bytes read from flash.
 * \param buffer_size                   Bytes available at \p buffer.
 * \param key_id                        CMAC key.
 * \param pnum                          Physical block the bytes came from.
 * \param erase_value                   Byte an erase leaves behind on this
 *                                      flash, from \c flash_get_parameters().
 * \param[out] header                   Decoded fields, written only on
 *                                      #UBI_HEADER_OK. May be \c NULL.
 *
 * \return Verdict of the checks.
 */
enum ubi_header_status
ubi_impl_header_ec_parse(const uint8_t *buffer, size_t buffer_size,
			 psa_key_id_t key_id, uint32_t pnum,
			 uint8_t erase_value, struct ubi_ec_header *header);

/**
 * \brief Serialize and seal a volume identifier header.
 *
 * \param[in] header                    Fields to encode.
 * \param key_id                        CMAC key.
 * \param pnum                          Physical block the header will live
 *                                      in; bound into the MAC.
 * \param[out] buffer                   Receives #UBI_HEADER_SIZE bytes.
 * \param buffer_size                   Bytes available at \p buffer.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         A pointer is \c NULL, \p key_id is \c PSA_KEY_ID_NULL, or
 *         \p buffer is too small.
 * \retval -EIO
 *         The crypto backend failed.
 */
int ubi_impl_header_vid_serialize(const struct ubi_vid_header *header,
				  psa_key_id_t key_id, uint32_t pnum,
				  uint8_t *buffer, size_t buffer_size);

/**
 * \brief Verify and decode a volume identifier header.
 *
 * \param[in] buffer                    Bytes read from flash.
 * \param buffer_size                   Bytes available at \p buffer.
 * \param key_id                        CMAC key.
 * \param pnum                          Physical block the bytes came from.
 * \param erase_value                   Byte an erase leaves behind on this
 *                                      flash, from \c flash_get_parameters().
 * \param[out] header                   Decoded fields, written only on
 *                                      #UBI_HEADER_OK. May be \c NULL.
 *
 * \return Verdict of the checks.
 */
enum ubi_header_status
ubi_impl_header_vid_parse(const uint8_t *buffer, size_t buffer_size,
			  psa_key_id_t key_id, uint32_t pnum,
			  uint8_t erase_value, struct ubi_vid_header *header);

/**
 * \brief Read and verify both headers of a physical erase block.
 *
 *        One flash read covers both, and the device supplies the key and the
 *        erased byte value, so a caller only names the block.
 *
 * \param[in] ubi                       Device holding the partition.
 * \param pnum                          Physical erase block to read.
 * \param[out] headers                  Verdicts and, where they are
 *                                      #UBI_HEADER_OK, the fields.
 *
 * \retval 0
 *         The bytes were read; \p headers says what they turned out to be.
 * \retval -EINVAL
 *         \p pnum is outside the partition.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_header_read(const struct ubi_device *ubi, uint32_t pnum,
			 struct ubi_headers *headers);

/**
 * \brief Seal an erase counter header and write it to a block.
 *
 *        The block must have been erased first; on NOR a second write over
 *        the same bytes destroys them.
 *
 * \param[in] ubi                       Device holding the partition.
 * \param pnum                          Physical erase block to stamp.
 * \param[in] header                    Fields to write.
 *
 * \retval 0
 *         Written.
 * \retval -EINVAL
 *         \p pnum is outside the partition.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_header_ec_write(struct ubi_device *ubi, uint32_t pnum,
			     const struct ubi_ec_header *header);

/**
 * \brief Seal a volume identifier header and write it to a block.
 *
 *        The block must already carry an erase counter header and nothing
 *        else.
 *
 * \param[in] ubi                       Device holding the partition.
 * \param pnum                          Physical erase block to write.
 * \param[in] header                    Fields to write.
 *
 * \retval 0
 *         Written.
 * \retval -EINVAL
 *         \p pnum is outside the partition.
 * \retval -EIO
 *         The crypto backend or the flash driver failed.
 */
int ubi_impl_header_vid_write(struct ubi_device *ubi, uint32_t pnum,
			      const struct ubi_vid_header *header);

/**
 * \brief Check a block's data area against what its VID header promises.
 *
 *        Only a header with \p copy_flag promises anything: it was sealed
 *        after its data was down, so a length and a checksum that do not
 *        match mean a power loss cut the write short. Without the flag the
 *        data area is the caller's business and this succeeds.
 *
 * \param[in] ubi                       Device holding the partition.
 * \param pnum                          Physical erase block to check.
 * \param[in] header                    Header that was read from it.
 *
 * \retval 0
 *         Intact, or nothing was promised.
 * \retval -EBADMSG
 *         The write was interrupted; the block carries partial data.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_header_vid_data_verify(const struct ubi_device *ubi, uint32_t pnum,
				    const struct ubi_vid_header *header);

#endif /* UBI_HEADER_H */
