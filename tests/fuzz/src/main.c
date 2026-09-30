/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   libFuzzer target for everything UBI decodes off the flash.
 *
 *          The first byte of an input picks a decoder: the erase counter
 *          header, the volume identifier header or the volume table record.
 *          The rest is read front to back and fed in three ways:
 *
 *          - as it stands, and for a header with its checksum made to match:
 *            nobody sealed it under the key, so it must never verify;
 *          - sealed as it stands under the test keys, so that the checks
 *            behind the MAC are reached: it has to decode, or be refused as
 *            another release's when this build cannot read it;
 *          - as fields the library seals itself: they have to decode to what
 *            went in.
 *
 *          Inputs arrive through an interrupt, as in Zephyr's fuzz sample.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* native_sim headers: */
#include <irq_ctrl.h>
#include <nsi_cpu_if.h>
#include <nsi_main_semipublic.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_header.h"
#include "ubi_key.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

/* Format constants, as docs/on-flash-format.md lays them out. */
#define HEADER_OFFSET_VERSION (0x04)
#define EC_MAGIC (0x55424923UL)
#define EC_OFFSET_VID_HEADER_OFFSET (0x10)
#define EC_OFFSET_DATA_OFFSET (0x14)
#define EC_OFFSET_MAC (0x1C)
#define VID_MAGIC (0x55424921UL)
#define VID_OFFSET_MAC (0x18)
#define RECORD_MAGIC (0x55424956UL)
#define RECORD_OFFSET_VERSION (0x04)
#define RECORD_OFFSET_VOLUME_COUNT (0x1C)

/** The checksum is the last word of either header. */
#define HEADER_CRC_OFFSET (UBI_HEADER_SIZE - sizeof(uint32_t))

/** Bytes a header's MAC is computed over: the block number, then the header
 *  without its MAC and its checksum. */
#define HEADER_MAC_INPUT_SIZE \
	(sizeof(uint32_t) + HEADER_CRC_OFFSET - UBI_MAC_SIZE)

/** Room for a record listing one volume more than this build allows. */
#define RECORD_ROOM \
	(UBI_VOLUME_TABLE_RECORD_MAX_SIZE + UBI_VOLUME_TABLE_ENTRY_SIZE)

/** Byte an erase leaves behind on the flash the decoders are told about. */
#define ERASE_VALUE (0xFF)

/** Decoders the first byte of an input picks from. */
#define DECODERS (3)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief An input, read front to back. Past its end it reads as zeros.
 */
struct fuzz_input {
	/** The bytes libFuzzer delivered. */
	const uint8_t *data;
	/** How many. */
	size_t size;
	/** How many have been taken. */
	size_t taken;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Stop the run in a way libFuzzer reports, with the input kept.
 */
static FUNC_NORETURN void broken(const char *what);

/**
 * \brief Take the next \p length bytes, zeros past the end of the input.
 */
static void input_take(struct fuzz_input *input, uint8_t *out, size_t length);

/**
 * \brief Take the next byte.
 */
static uint8_t input_take_u8(struct fuzz_input *input);

/**
 * \brief Take the next two bytes, big-endian.
 */
static uint16_t input_take_be16(struct fuzz_input *input);

/**
 * \brief Take the next four bytes, big-endian.
 */
static uint32_t input_take_be32(struct fuzz_input *input);

/**
 * \brief Take the next eight bytes, big-endian.
 */
static uint64_t input_take_be64(struct fuzz_input *input);

/**
 * \brief Recompute a header's checksum over the bytes in front of it.
 */
static void header_crc_fix(uint8_t *buffer);

/**
 * \brief Seal a header as it stands, whatever its fields say: set its magic,
 *        then its MAC, then its checksum.
 */
static void header_reseal(uint8_t *buffer, uint32_t magic, size_t mac_offset,
			  uint32_t pnum);

/**
 * \brief Stop the run if a header nobody sealed got past the MAC: the
 *        version and the layout are only looked at behind it.
 */
static void header_never_verifies(enum ubi_header_status status,
				  const char *what);

/**
 * \brief Fail unless something sealed as it stands decodes when this build
 *        can read it, and is refused as another release's when it cannot.
 */
static void resealed_check(enum ubi_header_status status, bool readable,
			   const char *what);

/**
 * \brief Feed an input to the erase counter header decoder.
 */
static void header_ec_fuzz(struct fuzz_input *input);

/**
 * \brief Feed an input to the volume identifier header decoder.
 */
static void header_vid_fuzz(struct fuzz_input *input);

/**
 * \brief Feed an input to the volume table record decoder.
 */
static void record_fuzz(struct fuzz_input *input);

/**
 * \brief Hand an input to the decoder its first byte picks.
 */
static void input_fuzz(const uint8_t *data, size_t size);

/**
 * \brief Wake the thread that decodes the input just delivered.
 */
static void fuzz_isr(const void *arg);

/* Module interface function declarations ---------------------------------- */

/**
 * \brief libFuzzer's entry point, called once for every input.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Module variables and constants ------------------------------------------ */

/** Keying material the keys are derived from. */
static const uint8_t ikm_bytes[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
	0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

static psa_key_id_t key_header = PSA_KEY_ID_NULL;
static psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;

/* The input libFuzzer delivered, and how many went through. */
static const uint8_t *fuzz_data = NULL;
static size_t fuzz_size = 0;
static uint32_t fuzzed = 0;

K_SEM_DEFINE(fuzz_sem, 0, 1);

/* Static function definitions --------------------------------------------- */

static FUNC_NORETURN void broken(const char *what)
{
	printk("broken: %s\n", what);
	__builtin_trap();
}

static void input_take(struct fuzz_input *input, uint8_t *out, size_t length)
{
	const size_t copied = MIN(length, input->size - input->taken);

	memcpy(out, &input->data[input->taken], copied);
	memset(&out[copied], 0, length - copied);
	input->taken += copied;
}

static uint8_t input_take_u8(struct fuzz_input *input)
{
	uint8_t value = 0;

	input_take(input, &value, sizeof(value));

	return value;
}

static uint16_t input_take_be16(struct fuzz_input *input)
{
	uint8_t bytes[sizeof(uint16_t)] = { 0 };

	input_take(input, bytes, sizeof(bytes));

	return sys_get_be16(bytes);
}

static uint32_t input_take_be32(struct fuzz_input *input)
{
	uint8_t bytes[sizeof(uint32_t)] = { 0 };

	input_take(input, bytes, sizeof(bytes));

	return sys_get_be32(bytes);
}

static uint64_t input_take_be64(struct fuzz_input *input)
{
	uint8_t bytes[sizeof(uint64_t)] = { 0 };

	input_take(input, bytes, sizeof(bytes));

	return sys_get_be64(bytes);
}

static void header_crc_fix(uint8_t *buffer)
{
	sys_put_be32(crc32_ieee(buffer, HEADER_CRC_OFFSET),
		     &buffer[HEADER_CRC_OFFSET]);
}

static void header_reseal(uint8_t *buffer, uint32_t magic, size_t mac_offset,
			  uint32_t pnum)
{
	const size_t tail = mac_offset + UBI_MAC_SIZE;
	uint8_t message[HEADER_MAC_INPUT_SIZE] = { 0 };
	size_t mac_length = 0;

	sys_put_be32(magic, buffer);

	sys_put_be32(pnum, message);
	memcpy(&message[sizeof(uint32_t)], buffer, mac_offset);
	memcpy(&message[sizeof(uint32_t) + mac_offset], &buffer[tail],
	       HEADER_CRC_OFFSET - tail);

	const psa_status_t status = psa_mac_compute(key_header, PSA_ALG_CMAC,
						    message, sizeof(message),
						    &buffer[mac_offset],
						    UBI_MAC_SIZE, &mac_length);

	if (PSA_SUCCESS != status)
		broken("a header would not take a MAC");

	header_crc_fix(buffer);
}

static void header_never_verifies(enum ubi_header_status status,
				  const char *what)
{
	if (UBI_HEADER_OK == status || UBI_HEADER_UNSUPPORTED == status)
		broken(what);
}

static void resealed_check(enum ubi_header_status status, bool readable,
			   const char *what)
{
	if (status != (readable ? UBI_HEADER_OK : UBI_HEADER_UNSUPPORTED))
		broken(what);
}

static void header_ec_fuzz(struct fuzz_input *input)
{
	const uint32_t pnum = input_take_be16(input);
	struct ubi_ec_header sealed = { 0 };
	struct ubi_ec_header decoded = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };
	enum ubi_header_status status = UBI_HEADER_ERROR;

	/* The rest as it would sit on the flash, whatever its length. */
	status = ubi_impl_header_ec_parse(&input->data[input->taken],
					  input->size - input->taken,
					  key_header, pnum, ERASE_VALUE,
					  &decoded);
	header_never_verifies(status, "a raw erase counter header");

	input_take(input, buffer, sizeof(buffer));
	header_crc_fix(buffer);
	status = ubi_impl_header_ec_parse(buffer, sizeof(buffer), key_header,
					  pnum, ERASE_VALUE, &decoded);
	header_never_verifies(status, "an erase counter header with its "
				      "checksum fixed");

	header_reseal(buffer, EC_MAGIC, EC_OFFSET_MAC, pnum);

	const bool readable =
		UBI_HEADER_VERSION == buffer[HEADER_OFFSET_VERSION] &&
		UBI_VID_HEADER_OFFSET ==
			sys_get_be32(&buffer[EC_OFFSET_VID_HEADER_OFFSET]) &&
		UBI_DATA_OFFSET == sys_get_be32(&buffer[EC_OFFSET_DATA_OFFSET]);

	status = ubi_impl_header_ec_parse(buffer, sizeof(buffer), key_header,
					  pnum, ERASE_VALUE, &decoded);
	resealed_check(status, readable,
		       "an erase counter header sealed as it stands");

	sealed.erase_count = input_take_be64(input);
	sealed.image_seq = input_take_be32(input);
	sealed.vid_header_offset = input_take_be32(input);
	sealed.data_offset = input_take_be32(input);

	/* Mostly the layout this build addresses, or little would decode. */
	const bool addressable = (0 == input_take_u8(input) % 2);

	if (addressable) {
		sealed.vid_header_offset = UBI_VID_HEADER_OFFSET;
		sealed.data_offset = UBI_DATA_OFFSET;
	}

	const int ret = ubi_impl_header_ec_serialize(&sealed, key_header, pnum,
						     buffer, sizeof(buffer));

	if (0 != ret)
		broken("an erase counter header would not seal");

	status = ubi_impl_header_ec_parse(buffer, sizeof(buffer), key_header,
					  pnum, ERASE_VALUE, &decoded);

	const bool known_layout = UBI_VID_HEADER_OFFSET ==
					  sealed.vid_header_offset &&
				  UBI_DATA_OFFSET == sealed.data_offset;

	if (!known_layout) {
		if (UBI_HEADER_UNSUPPORTED != status)
			broken("an erase counter header of another layout");
		return;
	}

	if (UBI_HEADER_OK != status)
		broken("a sealed erase counter header would not decode");

	if (sealed.erase_count != decoded.erase_count ||
	    sealed.image_seq != decoded.image_seq ||
	    sealed.vid_header_offset != decoded.vid_header_offset ||
	    sealed.data_offset != decoded.data_offset)
		broken("an erase counter header came back changed");
}

static void header_vid_fuzz(struct fuzz_input *input)
{
	const uint32_t pnum = input_take_be16(input);
	struct ubi_vid_header sealed = { 0 };
	struct ubi_vid_header decoded = { 0 };
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };
	enum ubi_header_status status = UBI_HEADER_ERROR;

	/* The rest as it would sit on the flash, whatever its length. */
	status = ubi_impl_header_vid_parse(&input->data[input->taken],
					   input->size - input->taken,
					   key_header, pnum, ERASE_VALUE,
					   &decoded);
	header_never_verifies(status, "a raw volume identifier header");

	input_take(input, buffer, sizeof(buffer));
	header_crc_fix(buffer);
	status = ubi_impl_header_vid_parse(buffer, sizeof(buffer), key_header,
					   pnum, ERASE_VALUE, &decoded);
	header_never_verifies(status, "a volume identifier header with its "
				      "checksum fixed");

	header_reseal(buffer, VID_MAGIC, VID_OFFSET_MAC, pnum);

	const bool readable = UBI_HEADER_VERSION ==
			      buffer[HEADER_OFFSET_VERSION];

	status = ubi_impl_header_vid_parse(buffer, sizeof(buffer), key_header,
					   pnum, ERASE_VALUE, &decoded);
	resealed_check(status, readable,
		       "a volume identifier header sealed as it stands");

	sealed.sqnum = input_take_be64(input);
	sealed.vol_id = input_take_be32(input);
	sealed.lnum = input_take_be32(input);
	sealed.image_seq = input_take_be32(input);
	sealed.data_size = input_take_be32(input);
	sealed.data_crc = input_take_be32(input);
	sealed.copy_flag = (0 != input_take_u8(input) % 2);

	const int ret = ubi_impl_header_vid_serialize(&sealed, key_header, pnum,
						      buffer, sizeof(buffer));

	if (0 != ret)
		broken("a volume identifier header would not seal");

	status = ubi_impl_header_vid_parse(buffer, sizeof(buffer), key_header,
					   pnum, ERASE_VALUE, &decoded);

	if (UBI_HEADER_OK != status)
		broken("a sealed volume identifier header would not decode");

	if (sealed.sqnum != decoded.sqnum || sealed.vol_id != decoded.vol_id ||
	    sealed.lnum != decoded.lnum ||
	    sealed.image_seq != decoded.image_seq ||
	    sealed.data_size != decoded.data_size ||
	    sealed.data_crc != decoded.data_crc ||
	    sealed.copy_flag != decoded.copy_flag)
		broken("a volume identifier header came back changed");
}

static void record_fuzz(struct fuzz_input *input)
{
	static struct ubi_volume_table_record sealed;
	static struct ubi_volume_table_record decoded;
	static uint8_t buffer[UBI_VOLUME_TABLE_RECORD_MAX_SIZE];
	static uint8_t raw[RECORD_ROOM];
	enum ubi_header_status status = UBI_HEADER_ERROR;
	size_t mac_length = 0;
	size_t record_size = 0;

	/* The rest as it would sit on the flash, whatever its length. */
	status = ubi_impl_volume_table_record_parse(&input->data[input->taken],
						    input->size - input->taken,
						    key_volume_table, &decoded);

	if (UBI_HEADER_OK == status)
		broken("a raw record");

	/* Sealed as it stands, listing up to one volume more than allowed. */
	const uint32_t count =
		input_take_u8(input) % (CONFIG_UBI_MAX_NR_OF_VOLUMES + 2);
	const size_t mac_offset = UBI_VOLUME_TABLE_PREAMBLE_SIZE +
				  count * UBI_VOLUME_TABLE_ENTRY_SIZE;

	input_take(input, raw, sizeof(raw));
	sys_put_be32(RECORD_MAGIC, raw);
	sys_put_be32(count, &raw[RECORD_OFFSET_VOLUME_COUNT]);

	const psa_status_t mac_status =
		psa_mac_compute(key_volume_table, PSA_ALG_CMAC, raw, mac_offset,
				&raw[mac_offset], UBI_MAC_SIZE, &mac_length);

	if (PSA_SUCCESS != mac_status)
		broken("a record would not take a MAC");

	const bool readable = CONFIG_UBI_MAX_NR_OF_VOLUMES >= count &&
			      UBI_VOLUME_TABLE_VERSION ==
				      raw[RECORD_OFFSET_VERSION];

	status = ubi_impl_volume_table_record_parse(
		raw, mac_offset + UBI_MAC_SIZE, key_volume_table, &decoded);
	resealed_check(status, readable, "a record sealed as it stands");

	memset(&sealed, 0, sizeof(sealed));
	sealed.revision = input_take_be32(input);
	sealed.image_seq = input_take_be32(input);
	sealed.peb_size = input_take_be32(input);
	sealed.peb_count = input_take_be32(input);
	sealed.vol_id_watermark = input_take_be32(input);
	sealed.volume_count =
		input_take_u8(input) % (CONFIG_UBI_MAX_NR_OF_VOLUMES + 1);

	for (uint32_t i = 0; i < sealed.volume_count; ++i) {
		struct ubi_volume_table_entry *entry = &sealed.entries[i];

		entry->vol_id = input_take_be32(input);
		entry->leb_count = input_take_be32(input);
		input_take(input, (uint8_t *)entry->name,
			   UBI_VOLUME_NAME_MAX_LEN);
	}

	const int ret = ubi_impl_volume_table_record_serialize(
		&sealed, key_volume_table, buffer, sizeof(buffer),
		&record_size);

	if (0 != ret)
		broken("a record would not seal");

	status = ubi_impl_volume_table_record_parse(buffer, record_size,
						    key_volume_table, &decoded);

	if (UBI_HEADER_OK != status)
		broken("a sealed record would not decode");

	if (sealed.revision != decoded.revision ||
	    sealed.image_seq != decoded.image_seq ||
	    sealed.peb_size != decoded.peb_size ||
	    sealed.peb_count != decoded.peb_count ||
	    sealed.vol_id_watermark != decoded.vol_id_watermark ||
	    sealed.volume_count != decoded.volume_count)
		broken("a record preamble came back changed");

	for (uint32_t i = 0; i < sealed.volume_count; ++i) {
		const struct ubi_volume_table_entry *in = &sealed.entries[i];
		const struct ubi_volume_table_entry *out = &decoded.entries[i];
		const bool same_name = (0 == strcmp(in->name, out->name));

		if (in->vol_id != out->vol_id ||
		    in->leb_count != out->leb_count || !same_name)
			broken("a record entry came back changed");
	}
}

static void input_fuzz(const uint8_t *data, size_t size)
{
	struct fuzz_input input = { .data = data, .size = size, .taken = 0 };

	if (0 == size)
		return;

	const uint8_t decoder = input_take_u8(&input);

	switch (decoder % DECODERS) {
	case 0:
		header_ec_fuzz(&input);
		break;
	case 1:
		header_vid_fuzz(&input);
		break;
	default:
		record_fuzz(&input);
		break;
	}
}

static void fuzz_isr(const void *arg)
{
	ARG_UNUSED(arg);

	k_sem_give(&fuzz_sem);
}

/* Module interface function definitions ----------------------------------- */

int main(void)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t ikm_key = PSA_KEY_ID_NULL;
	psa_status_t status = psa_crypto_init();

	if (PSA_SUCCESS != status) {
		printk("no PSA crypto\n");
		return -1;
	}

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));

	status = psa_import_key(&attributes, ikm_bytes, sizeof(ikm_bytes),
				&ikm_key);

	if (PSA_SUCCESS != status) {
		printk("no keying material\n");
		return -1;
	}

	const int ret = ubi_impl_key_derive(ikm_key, NULL, 0, &key_header,
					    &key_volume_table);

	if (0 != ret) {
		printk("no keys to seal with\n");
		return -1;
	}

	IRQ_CONNECT(CONFIG_ARCH_POSIX_FUZZ_IRQ, 0, fuzz_isr, NULL, 0);
	irq_enable(CONFIG_ARCH_POSIX_FUZZ_IRQ);

	while (true) {
		k_sem_take(&fuzz_sem, K_FOREVER);

		input_fuzz(fuzz_data, fuzz_size);

		fuzzed += 1;

		if (0 == fuzzed % CONFIG_UBI_TEST_FUZZ_REPORT_EVERY)
			printk("fuzzed %u inputs\n", fuzzed);
	}
}

/**
 * Called by libFuzzer for every input: hand it over through the interrupt
 * and let the simulated CPU run until the input has been decoded.
 */
NATIVE_SIMULATOR_IF int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static bool runner_initialized = false;

	if (!runner_initialized) {
		nsi_init(0, NULL);
		runner_initialized = true;
	}

	fuzz_data = data;
	fuzz_size = size;

	hw_irq_ctrl_set_irq(CONFIG_ARCH_POSIX_FUZZ_IRQ);

	nsi_exec_for(k_ticks_to_us_ceil64(CONFIG_ARCH_POSIX_FUZZ_TICKS));

	return 0;
}
