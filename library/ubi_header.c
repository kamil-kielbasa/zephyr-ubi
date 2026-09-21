/**
 * \file    ubi_header.c
 * \author  Kamil Kielbasa
 * \brief   On-flash header serialization and authentication.
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
#include "ubi_header.h"
#include "ubi_io.h"
#include "ubi_key.h"
#include "ubi_private.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_REGISTER(ubi, CONFIG_UBI_LOG_LEVEL);

/** "UBI#" - erase counter header, the value Linux UBI uses. */
#define UBI_EC_HEADER_MAGIC (0x55424923UL)

/** "UBI!" - volume identifier header, the value Linux UBI uses. */
#define UBI_VID_HEADER_MAGIC (0x55424921UL)

/** Only dynamic volumes exist; static ones were dropped by design. */
#define UBI_VID_TYPE_DYNAMIC (1)

/** Bytes read at a time when checksumming a block's data area. */
#define DATA_VERIFY_CHUNK (128)

/*
 * Field offsets. Every field Linux UBI interprets sits where Linux puts it;
 * the MAC occupies bytes Linux reserves as padding.
 */

#define HEADER_OFFSET_MAGIC (0x00)
#define HEADER_OFFSET_VERSION (0x04)
#define HEADER_OFFSET_CRC (0x3C)

#define EC_OFFSET_ERASE_COUNT (0x08)
#define EC_OFFSET_VID_HEADER_OFFSET (0x10)
#define EC_OFFSET_DATA_OFFSET (0x14)
#define EC_OFFSET_IMAGE_SEQ (0x18)
#define EC_OFFSET_MAC (0x1C)

#define VID_OFFSET_VOL_TYPE (0x05)
#define VID_OFFSET_COPY_FLAG (0x06)
#define VID_OFFSET_COMPAT (0x07)
#define VID_OFFSET_VOL_ID (0x08)
#define VID_OFFSET_LNUM (0x0C)
#define VID_OFFSET_IMAGE_SEQ (0x10)
#define VID_OFFSET_DATA_SIZE (0x14)
#define VID_OFFSET_MAC (0x18)
#define VID_OFFSET_SQNUM (0x28)
#define VID_OFFSET_DATA_CRC (0x30)

/**
 * Size of the byte sequence the MAC is computed over: the block number
 * followed by the header with the MAC and the CRC cut out. Both headers place
 * their MAC such that this works out to three AES blocks.
 */
#define HEADER_MAC_INPUT_SIZE \
	(sizeof(uint32_t) + HEADER_OFFSET_CRC - UBI_MAC_SIZE)

BUILD_ASSERT(HEADER_MAC_INPUT_SIZE == 48,
	     "authenticated input must be three AES blocks");

BUILD_ASSERT(EC_OFFSET_MAC + UBI_MAC_SIZE <= HEADER_OFFSET_CRC,
	     "EC MAC overlaps the CRC");

BUILD_ASSERT(VID_OFFSET_MAC + UBI_MAC_SIZE <= HEADER_OFFSET_CRC,
	     "VID MAC overlaps the CRC");

BUILD_ASSERT(UBI_DATA_OFFSET == UBI_VID_HEADER_OFFSET + UBI_HEADER_SIZE,
	     "the two headers must be adjacent so one read fetches both");

/* Static function declarations -------------------------------------------- */

/**
 * \brief Lay out the exact byte sequence that the MAC is computed over.
 *
 *        That sequence is the physical block number in big-endian, followed
 *        by the header with two windows removed: the MAC itself, which cannot
 *        authenticate itself, and the trailing CRC, which is computed
 *        afterwards and so is not known yet.
 *
 *        Prefixing the block number is what binds a header to its position.
 *        The same bytes read from a different block produce a different input
 *        here, so the stored MAC no longer matches.
 *
 * \param[in] buffer                    Header bytes; the MAC field is
 *                                      skipped, so its content is ignored.
 * \param buffer_size                   Bytes available at \p buffer.
 * \param pnum                          Physical block number to bind in.
 * \param mac_offset                    Where the MAC sits in \p buffer.
 * \param[out] message                  Receives the authenticated input.
 * \param message_size                  Bytes available at \p message.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         Either buffer is too small.
 */
static int header_build_mac_input(const uint8_t *buffer, size_t buffer_size,
				  uint32_t pnum, size_t mac_offset,
				  uint8_t *message, size_t message_size);

/**
 * \brief Compute the MAC over the fields, then the CRC that covers it.
 *
 * \param key_id                        CMAC key.
 * \param pnum                          Physical block number to bind in.
 * \param mac_offset                    Where the MAC sits in \p buffer.
 * \param[in,out] buffer                Fields in, MAC and CRC filled in.
 * \param buffer_size                   Bytes available at \p buffer.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p buffer is too small.
 * \retval -EIO
 *         The crypto backend failed.
 */
static int header_seal(psa_key_id_t key_id, uint32_t pnum, size_t mac_offset,
		       uint8_t *buffer, size_t buffer_size);

/**
 * \brief Run the magic, CRC and MAC checks, in that order, and say what
 *        failed.
 *
 * \param[in] buffer                    Header bytes read from flash.
 * \param buffer_size                   Bytes available at \p buffer.
 * \param key_id                        CMAC key.
 * \param pnum                          Physical block the bytes came from.
 * \param magic                         Magic the header must carry.
 * \param mac_offset                    Where the MAC sits in \p buffer.
 * \param erase_value                   Byte an erase leaves behind.
 *
 * \return Verdict of the checks.
 */
static enum ubi_header_status header_verify(const uint8_t *buffer,
					    size_t buffer_size,
					    psa_key_id_t key_id, uint32_t pnum,
					    uint32_t magic, size_t mac_offset,
					    uint8_t erase_value);

/**
 * \brief Name the header a magic belongs to, for logging.
 */
static const char *header_name(uint32_t magic);

/**
 * \brief Clear a header buffer and lay down the magic and the version.
 */
static void header_begin(uint8_t *buffer, uint32_t magic);

/**
 * \brief Report whether every byte reads as the flash's erased value.
 */
static bool header_is_erased(const uint8_t *buffer, uint8_t erase_value);

/**
 * \brief Reject arguments that cannot produce a valid header, and say so.
 */
static bool header_arguments_are_usable(const uint8_t *buffer,
					size_t buffer_size, psa_key_id_t key_id,
					uint32_t pnum, uint32_t magic);

/* Static function definitions --------------------------------------------- */

static int header_build_mac_input(const uint8_t *buffer, size_t buffer_size,
				  uint32_t pnum, size_t mac_offset,
				  uint8_t *message, size_t message_size)
{
	/* Where the header resumes once the MAC has been stepped over. */
	const size_t tail = mac_offset + UBI_MAC_SIZE;

	/* The block number leads, then the header in two pieces: everything
	 * before the MAC and everything between it and the CRC. The MAC
	 * cannot authenticate itself, and the CRC is computed afterwards. */
	const size_t before = sizeof(uint32_t);
	const size_t after = before + mac_offset;

	if (UBI_HEADER_SIZE > buffer_size ||
	    HEADER_MAC_INPUT_SIZE > message_size) {
		return -EINVAL;
	}

	sys_put_be32(pnum, &message[0]);
	memcpy(&message[before], &buffer[0], mac_offset);
	memcpy(&message[after], &buffer[tail], HEADER_OFFSET_CRC - tail);

	return 0;
}

static int header_seal(psa_key_id_t key_id, uint32_t pnum, size_t mac_offset,
		       uint8_t *buffer, size_t buffer_size)
{
	uint8_t message[HEADER_MAC_INPUT_SIZE] = { 0 };
	size_t mac_length = 0;
	const int ret = header_build_mac_input(buffer, buffer_size, pnum,
					       mac_offset, message,
					       sizeof(message));

	if (0 != ret) {
		LOG_ERR("PEB %u: %s header does not fit %zu bytes", pnum,
			header_name(sys_get_be32(&buffer[HEADER_OFFSET_MAGIC])),
			buffer_size);
		return ret;
	}

	const psa_status_t status =
		psa_mac_compute(key_id, PSA_ALG_CMAC, message, sizeof(message),
				&buffer[mac_offset], UBI_MAC_SIZE, &mac_length);

	if (PSA_SUCCESS != status || UBI_MAC_SIZE != mac_length) {
		LOG_ERR("PEB %u: %s header could not be sealed, psa_status=%d",
			pnum,
			header_name(sys_get_be32(&buffer[HEADER_OFFSET_MAGIC])),
			(int)status);
		return -EIO;
	}

	sys_put_be32(crc32_ieee(buffer, HEADER_OFFSET_CRC),
		     &buffer[HEADER_OFFSET_CRC]);

	return 0;
}

static enum ubi_header_status header_verify(const uint8_t *buffer,
					    size_t buffer_size,
					    psa_key_id_t key_id, uint32_t pnum,
					    uint32_t magic, size_t mac_offset,
					    uint8_t erase_value)
{
	uint8_t message[HEADER_MAC_INPUT_SIZE] = { 0 };
	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	int ret = 0;

	if (header_is_erased(buffer, erase_value)) {
		return UBI_HEADER_ERASED;
	}

	if (magic != sys_get_be32(&buffer[HEADER_OFFSET_MAGIC])) {
		return UBI_HEADER_NOT_UBI;
	}

	if (sys_get_be32(&buffer[HEADER_OFFSET_CRC]) !=
	    crc32_ieee(buffer, HEADER_OFFSET_CRC)) {
		LOG_ERR("PEB %u: %s header CRC mismatch, so it is unusable",
			pnum, header_name(magic));
		return UBI_HEADER_CORRUPT;
	}

	ret = header_build_mac_input(buffer, buffer_size, pnum, mac_offset,
				     message, sizeof(message));

	if (0 != ret) {
		return UBI_HEADER_ERROR;
	}

	/* Constant-time comparison; never memcmp() a MAC. */
	status = psa_mac_verify(key_id, PSA_ALG_CMAC, message, sizeof(message),
				&buffer[mac_offset], UBI_MAC_SIZE);

	if (PSA_ERROR_INVALID_SIGNATURE == status) {
		LOG_ERR("PEB %u: %s header CRC matches but the MAC does not, "
			"so it was modified",
			pnum, header_name(magic));
		return UBI_HEADER_TAMPERED;
	}

	if (PSA_SUCCESS != status) {
		LOG_ERR("PEB %u: %s header could not be checked, psa_status=%d",
			pnum, header_name(magic), (int)status);
		return UBI_HEADER_ERROR;
	}

	/* Only now is the version byte trustworthy. */
	if (UBI_HEADER_VERSION != buffer[HEADER_OFFSET_VERSION])
		return UBI_HEADER_NOT_UBI;

	return UBI_HEADER_OK;
}

static const char *header_name(uint32_t magic)
{
	return (UBI_EC_HEADER_MAGIC == magic) ? "EC" : "VID";
}

static void header_begin(uint8_t *buffer, uint32_t magic)
{
	memset(buffer, 0, UBI_HEADER_SIZE);
	sys_put_be32(magic, &buffer[HEADER_OFFSET_MAGIC]);
	buffer[HEADER_OFFSET_VERSION] = UBI_HEADER_VERSION;
}

static bool header_is_erased(const uint8_t *buffer, uint8_t erase_value)
{
	for (size_t i = 0; i < UBI_HEADER_SIZE; ++i) {
		if (erase_value != buffer[i]) {
			return false;
		}
	}

	return true;
}

static bool header_arguments_are_usable(const uint8_t *buffer,
					size_t buffer_size, psa_key_id_t key_id,
					uint32_t pnum, uint32_t magic)
{
	if ((NULL != buffer) && (UBI_HEADER_SIZE <= buffer_size) &&
	    (PSA_KEY_ID_NULL != key_id))
		return true;

	LOG_ERR("PEB %u: %s header got bad arguments (buffer_size=%zu, "
		"key=%u)",
		pnum, header_name(magic), buffer_size, (unsigned int)key_id);

	return false;
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_header_ec_serialize(const struct ubi_ec_header *header,
				 psa_key_id_t key_id, uint32_t pnum,
				 uint8_t *buffer, size_t buffer_size)
{
	if (NULL == header ||
	    !header_arguments_are_usable(buffer, buffer_size, key_id, pnum,
					 UBI_EC_HEADER_MAGIC))
		return -EINVAL;

	header_begin(buffer, UBI_EC_HEADER_MAGIC);

	sys_put_be64(header->erase_count, &buffer[EC_OFFSET_ERASE_COUNT]);
	sys_put_be32(header->vid_header_offset,
		     &buffer[EC_OFFSET_VID_HEADER_OFFSET]);
	sys_put_be32(header->data_offset, &buffer[EC_OFFSET_DATA_OFFSET]);
	sys_put_be32(header->image_seq, &buffer[EC_OFFSET_IMAGE_SEQ]);

	return header_seal(key_id, pnum, EC_OFFSET_MAC, buffer, buffer_size);
}

enum ubi_header_status
ubi_impl_header_ec_parse(const uint8_t *buffer, size_t buffer_size,
			 psa_key_id_t key_id, uint32_t pnum,
			 uint8_t erase_value, struct ubi_ec_header *header)
{
	if (!header_arguments_are_usable(buffer, buffer_size, key_id, pnum,
					 UBI_EC_HEADER_MAGIC))
		return UBI_HEADER_ERROR;

	const enum ubi_header_status status =
		header_verify(buffer, buffer_size, key_id, pnum,
			      UBI_EC_HEADER_MAGIC, EC_OFFSET_MAC, erase_value);

	if (UBI_HEADER_OK != status)
		return status;

	/*
	 * The block layout is authentic at this point, but it still has to be
	 * the layout this build knows how to address.
	 */
	const uint32_t vid_header_offset =
		sys_get_be32(&buffer[EC_OFFSET_VID_HEADER_OFFSET]);
	const uint32_t data_offset =
		sys_get_be32(&buffer[EC_OFFSET_DATA_OFFSET]);

	if (UBI_VID_HEADER_OFFSET != vid_header_offset ||
	    UBI_DATA_OFFSET != data_offset) {
		LOG_WRN("PEB %u: authentic EC header describes an unsupported "
			"layout (vid=%u, data=%u)",
			pnum, vid_header_offset, data_offset);
		return UBI_HEADER_NOT_UBI;
	}

	if (NULL == header)
		return UBI_HEADER_OK;

	header->erase_count = sys_get_be64(&buffer[EC_OFFSET_ERASE_COUNT]);
	header->vid_header_offset = vid_header_offset;
	header->data_offset = data_offset;
	header->image_seq = sys_get_be32(&buffer[EC_OFFSET_IMAGE_SEQ]);

	return UBI_HEADER_OK;
}

int ubi_impl_header_vid_serialize(const struct ubi_vid_header *header,
				  psa_key_id_t key_id, uint32_t pnum,
				  uint8_t *buffer, size_t buffer_size)
{
	if (NULL == header ||
	    !header_arguments_are_usable(buffer, buffer_size, key_id, pnum,
					 UBI_VID_HEADER_MAGIC))
		return -EINVAL;

	header_begin(buffer, UBI_VID_HEADER_MAGIC);

	buffer[VID_OFFSET_VOL_TYPE] = UBI_VID_TYPE_DYNAMIC;
	buffer[VID_OFFSET_COPY_FLAG] = header->copy_flag ? 1 : 0;
	buffer[VID_OFFSET_COMPAT] = 0;
	sys_put_be32(header->vol_id, &buffer[VID_OFFSET_VOL_ID]);
	sys_put_be32(header->lnum, &buffer[VID_OFFSET_LNUM]);
	sys_put_be32(header->image_seq, &buffer[VID_OFFSET_IMAGE_SEQ]);
	sys_put_be32(header->data_size, &buffer[VID_OFFSET_DATA_SIZE]);
	sys_put_be64(header->sqnum, &buffer[VID_OFFSET_SQNUM]);
	sys_put_be32(header->data_crc, &buffer[VID_OFFSET_DATA_CRC]);

	return header_seal(key_id, pnum, VID_OFFSET_MAC, buffer, buffer_size);
}

enum ubi_header_status
ubi_impl_header_vid_parse(const uint8_t *buffer, size_t buffer_size,
			  psa_key_id_t key_id, uint32_t pnum,
			  uint8_t erase_value, struct ubi_vid_header *header)
{
	if (!header_arguments_are_usable(buffer, buffer_size, key_id, pnum,
					 UBI_VID_HEADER_MAGIC))
		return UBI_HEADER_ERROR;

	const enum ubi_header_status status = header_verify(
		buffer, buffer_size, key_id, pnum, UBI_VID_HEADER_MAGIC,
		VID_OFFSET_MAC, erase_value);

	if (UBI_HEADER_OK != status)
		return status;

	if (NULL == header)
		return UBI_HEADER_OK;

	header->sqnum = sys_get_be64(&buffer[VID_OFFSET_SQNUM]);
	header->vol_id = sys_get_be32(&buffer[VID_OFFSET_VOL_ID]);
	header->lnum = sys_get_be32(&buffer[VID_OFFSET_LNUM]);
	header->image_seq = sys_get_be32(&buffer[VID_OFFSET_IMAGE_SEQ]);
	header->data_size = sys_get_be32(&buffer[VID_OFFSET_DATA_SIZE]);
	header->data_crc = sys_get_be32(&buffer[VID_OFFSET_DATA_CRC]);
	header->copy_flag = (0 != buffer[VID_OFFSET_COPY_FLAG]);

	return UBI_HEADER_OK;
}

int ubi_impl_header_read(const struct ubi_device *ubi, uint32_t pnum,
			 struct ubi_headers *headers)
{
	uint8_t buffer[UBI_DATA_OFFSET] = { 0 };
	int ret = ubi_impl_io_read(ubi, pnum, 0, buffer, sizeof(buffer));

	if (0 != ret) {
		LOG_ERR("PEB %u: the headers could not be read (%d)", pnum,
			ret);
		return ret;
	}

	memset(headers, 0, sizeof(*headers));

	headers->ec_status = ubi_impl_header_ec_parse(
		&buffer[UBI_EC_HEADER_OFFSET], UBI_HEADER_SIZE,
		ubi->keys.header, pnum, ubi->geometry.erase_value,
		&headers->ec);

	headers->vid_status = ubi_impl_header_vid_parse(
		&buffer[UBI_VID_HEADER_OFFSET], UBI_HEADER_SIZE,
		ubi->keys.header, pnum, ubi->geometry.erase_value,
		&headers->vid);

	return 0;
}

int ubi_impl_header_ec_write(struct ubi_device *ubi, uint32_t pnum,
			     const struct ubi_ec_header *header)
{
	uint8_t buffer[UBI_HEADER_SIZE];
	const int ret = ubi_impl_header_ec_serialize(
		header, ubi->keys.header, pnum, buffer, sizeof(buffer));

	if (0 != ret)
		return ret;

	return ubi_impl_io_write(ubi, pnum, UBI_EC_HEADER_OFFSET, buffer,
				 sizeof(buffer));
}

int ubi_impl_header_vid_write(struct ubi_device *ubi, uint32_t pnum,
			      const struct ubi_vid_header *header)
{
	uint8_t buffer[UBI_HEADER_SIZE];
	const int ret = ubi_impl_header_vid_serialize(
		header, ubi->keys.header, pnum, buffer, sizeof(buffer));

	if (0 != ret)
		return ret;

	return ubi_impl_io_write(ubi, pnum, UBI_VID_HEADER_OFFSET, buffer,
				 sizeof(buffer));
}

int ubi_impl_header_vid_data_verify(const struct ubi_device *ubi, uint32_t pnum,
				    const struct ubi_vid_header *header)
{
	if (!header->copy_flag)
		return 0;

	if (header->data_size > ubi->geometry.leb_size) {
		LOG_ERR("PEB %u: claims %u bytes of data, more than the %u a "
			"logical block holds",
			pnum, header->data_size, ubi->geometry.leb_size);
		return -EBADMSG;
	}

	uint8_t chunk[DATA_VERIFY_CHUNK] = { 0 };
	uint32_t crc = 0;

	for (uint32_t at = 0; at < header->data_size; at += sizeof(chunk)) {
		const size_t length =
			MIN(sizeof(chunk), header->data_size - at);
		const int ret = ubi_impl_io_read(
			ubi, pnum, UBI_DATA_OFFSET + at, chunk, length);

		if (0 != ret)
			return ret;

		crc = crc32_ieee_update(crc, chunk, length);
	}

	if (crc != header->data_crc)
		return -EBADMSG;

	return 0;
}
