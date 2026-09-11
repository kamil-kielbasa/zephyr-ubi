/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   Integration tests for formatting and attaching.
 *
 *          Everything here goes through the public API and a simulated
 *          flash. The library carries no test hooks; where a test needs to
 *          see the flash it opens the partition itself, exactly as an
 *          attacker or a stray writer would.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Module defines ---------------------------------------------------------- */

#define TEST_PARTITION FIXED_PARTITION_ID(storage_partition)

/** Chunk used when sweeping the whole partition. */
#define SWEEP_CHUNK (256)

/* Module variables and constants ------------------------------------------ */

static struct ubi_device *ubi;

static const uint8_t ikm_bytes[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
	0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

static psa_key_id_t key_right;
static psa_key_id_t key_wrong;

static struct ubi_config config;
static struct ubi_config config_wrong_key;

static uint32_t event_count;
static enum ubi_event_type last_event;
static bool event_seen[UBI_EVENT_PEB_BAD + 1];
static uint32_t state_check_count;
static struct ubi_device_info last_state;

/* Static function definitions --------------------------------------------- */

static enum ubi_state_verdict
trust_everything(const struct ubi_device_info *info, void *user_context)
{
	ARG_UNUSED(user_context);

	state_check_count += 1;
	last_state = *info;

	return UBI_STATE_TRUSTED;
}

static enum ubi_state_verdict trust_nothing(const struct ubi_device_info *info,
					    void *user_context)
{
	ARG_UNUSED(user_context);

	state_check_count += 1;
	last_state = *info;

	return UBI_STATE_UNTRUSTED;
}

static void on_event(const struct ubi_event *event, void *user_context)
{
	ARG_UNUSED(user_context);

	event_count += 1;
	last_event = event->type;
	event_seen[event->type] = true;
}

static psa_key_id_t import_ikm(const uint8_t *bytes, size_t length)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key_id = PSA_KEY_ID_NULL;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);

	zassert_equal(PSA_SUCCESS,
		      psa_import_key(&attributes, bytes, length, &key_id));

	return key_id;
}

/**
 * \brief Overwrite the whole partition with one byte value.
 */
static void partition_fill(uint8_t value)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK];

	memset(chunk, value, sizeof(chunk));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));
	zassert_ok(flash_area_erase(flash_area, 0, flash_area->fa_size));

	if (0xFF != value) {
		for (off_t at = 0; at < (off_t)flash_area->fa_size;
		     at += sizeof(chunk)) {
			zassert_ok(flash_area_write(flash_area, at, chunk,
						    sizeof(chunk)));
		}
	}

	flash_area_close(flash_area);
}

/**
 * \brief Fingerprint the partition, so a test can prove nothing moved.
 */
static uint32_t partition_fingerprint(void)
{
	const struct flash_area *flash_area = NULL;
	uint8_t chunk[SWEEP_CHUNK];
	uint32_t crc = 0;

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	for (off_t at = 0; at < (off_t)flash_area->fa_size;
	     at += sizeof(chunk)) {
		zassert_ok(
			flash_area_read(flash_area, at, chunk, sizeof(chunk)));
		crc = crc32_ieee_update(crc, chunk, sizeof(chunk));
	}

	flash_area_close(flash_area);

	return crc;
}

/**
 * \brief Flip one byte in the first \p copies volume table records found.
 *
 * \return How many records were reached.
 */
static uint32_t corrupt_volume_tables(uint32_t copies)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };
	uint8_t byte = 0;
	uint32_t damaged = 0;

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	/* The record sits behind the two headers of whichever block holds it;
	 * clearing the lowest set bit is a write NOR always allows. */
	for (uint32_t pnum = 0; pnum < info.peb_count && damaged < copies;
	     ++pnum) {
		const off_t at = (off_t)pnum * info.peb_size + 128;

		zassert_ok(flash_area_read(flash_area, at, &byte, 1));

		if (0xFF != byte && 0x00 != byte) {
			byte &= (uint8_t)(byte - 1U);
			zassert_ok(flash_area_write(flash_area, at, &byte, 1));
			damaged += 1;
		}
	}

	flash_area_close(flash_area);

	return damaged;
}

static void *suite_setup(void)
{
	zassert_equal(PSA_SUCCESS, psa_crypto_init());

	key_right = import_ikm(ikm_bytes, sizeof(ikm_bytes));

	uint8_t other[sizeof(ikm_bytes)];

	memcpy(other, ikm_bytes, sizeof(other));
	other[0] ^= 0xFF;
	key_wrong = import_ikm(other, sizeof(other));

	config.flash_area_id = TEST_PARTITION;
	config.ikm_key_id = key_right;
	config.event_cb = on_event;
	config.state_cb = trust_everything;

	config_wrong_key = config;
	config_wrong_key.ikm_key_id = key_wrong;

	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);

	event_count = 0;
	state_check_count = 0;
	memset(event_seen, 0, sizeof(event_seen));
	memset(&last_state, 0, sizeof(last_state));

	ubi = k_malloc(ubi_device_size());
	zassert_not_null(ubi, "no memory for a device handle");
	memset(ubi, 0, ubi_device_size());

	partition_fill(0xFF);
}

static void after_each(void *fixture)
{
	ARG_UNUSED(fixture);

	k_free(ubi);
	ubi = NULL;
}

ZTEST_SUITE(ubi_integration, NULL, suite_setup, before_each, after_each, NULL);

/* Tests: recognising what is on the flash --------------------------------- */

ZTEST(ubi_integration, test_blank_partition_is_not_a_ubi_device)
{
	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

ZTEST(ubi_integration, test_foreign_content_is_not_a_ubi_device)
{
	partition_fill(0x5A);

	/* Refusing beats formatting: the bytes might be someone's data. */
	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

ZTEST(ubi_integration, test_format_then_attach)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(128, info.peb_count);
	zassert_equal(4096, info.peb_size);
	zassert_equal(4096 - 128, info.leb_size,
		      "two headers precede the data");
	zassert_equal(1, info.write_block_size);
	zassert_equal(0, info.volume_count);
	zassert_not_equal(0, info.image_seq);
	zassert_equal(1, info.revision);
	zassert_equal(0, event_count, "a clean attach has nothing to report");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_attach_survives_a_reboot)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	/* Detaching and attaching again is what a reboot looks like. */
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(first.image_seq, second.image_seq);
	zassert_equal(first.revision, second.revision);
	zassert_equal(first.global_sqnum, second.global_sqnum);
	zassert_equal(first.total_erase_count, second.total_erase_count);
	zassert_equal(first.healthy_pebs, second.healthy_pebs);
}

ZTEST(ubi_integration, test_reformatting_starts_a_new_image)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_not_equal(first.image_seq, second.image_seq,
			  "a reformat must not reuse the image sequence");
}

/* Tests: attach never damages the flash ----------------------------------- */

ZTEST(ubi_integration, test_a_wrong_key_is_refused_without_damage)
{
	uint32_t before = 0;
	uint32_t after = 0;

	zassert_ok(ubi_device_format(&config));

	before = partition_fingerprint();
	zassert_equal(-EBADMSG, ubi_device_init(ubi, &config_wrong_key));
	after = partition_fingerprint();

	zassert_equal(before, after,
		      "attach must not write, or a typo would destroy data");

	/* The right key still opens it afterwards. */
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_attach_leaves_the_flash_untouched)
{
	uint32_t before = 0;
	uint32_t after = 0;

	zassert_ok(ubi_device_format(&config));

	before = partition_fingerprint();
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
	after = partition_fingerprint();

	zassert_equal(before, after);
}

ZTEST(ubi_integration, test_failed_attach_on_blank_flash_writes_nothing)
{
	const uint32_t before = partition_fingerprint();

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));

	zassert_equal(before, partition_fingerprint());
}

/* Tests: damaged and tampered metadata ------------------------------------ */

ZTEST(ubi_integration, test_one_damaged_volume_table_copy_is_survived)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_volume_tables(1));

	/* This is what the second copy is for, but the device is now one
	 * erase away from losing a revision and has to say so. */
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(before.image_seq, after.image_seq);
	zassert_equal(before.revision, after.revision);
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED]);
}

ZTEST(ubi_integration, test_both_damaged_volume_table_copies_lose_the_device)
{
	zassert_ok(ubi_device_format(&config));
	zassert_equal(2, corrupt_volume_tables(2));

	/*
	 * The record carries a checksum written before it, so a changed byte
	 * is indistinguishable from a write that never finished. Either way
	 * there is no usable volume table left, which is what attach reports.
	 */
	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

ZTEST(ubi_integration, test_erasing_every_stamped_block_loses_the_device)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	for (uint32_t pnum = 0; pnum < info.peb_count; ++pnum) {
		uint8_t magic[4] = { 0 };

		zassert_ok(flash_area_read(flash_area,
					   (off_t)pnum * info.peb_size, magic,
					   sizeof(magic)));

		if (0xFF == magic[0])
			continue;

		zassert_ok(flash_area_erase(flash_area,
					    (off_t)pnum * info.peb_size,
					    info.peb_size));
	}

	flash_area_close(flash_area);

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

ZTEST(ubi_integration, test_a_damaged_erase_counter_header_is_reported)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };
	uint8_t byte = 0;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(flash_area_open(TEST_PARTITION, &flash_area));

	/* A freshly stamped block has an erase count of one, so the last byte
	 * of that field has a bit NOR still allows clearing. */
	for (uint32_t pnum = 0; pnum < info.peb_count; ++pnum) {
		const off_t at = (off_t)pnum * info.peb_size + 0x0F;

		zassert_ok(flash_area_read(flash_area, at, &byte, 1));

		if (0x01 == byte) {
			byte = 0x00;
			zassert_ok(flash_area_write(flash_area, at, &byte, 1));
			break;
		}
	}

	flash_area_close(flash_area);

	event_count = 0;
	memset(event_seen, 0, sizeof(event_seen));

	/* The other copy carries the device through, but the damage is still
	 * reported. Nobody repaired the checksum, so it is damage rather than
	 * tampering. */
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_true(event_seen[UBI_EVENT_HDR_CORRUPT]);
	zassert_true(event_seen[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		     "one copy left has to be reported as such");
	zassert_false(event_seen[UBI_EVENT_HDR_TAMPERED]);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.healthy_pebs, "the damaged block is not counted");

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: the handle contract ---------------------------------------------- */

ZTEST(ubi_integration, test_attaching_twice_is_refused)
{
	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EBUSY, ubi_device_init(ubi, &config));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_detached_handle_answers_nothing)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(-EINVAL, ubi_device_get_info(ubi, &info));
	zassert_equal(-EINVAL, ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_the_handle_has_a_size_the_caller_can_allocate)
{
	zassert_true(0 != ubi_device_size());
	zassert_true(sizeof(struct ubi_config) < ubi_device_size(),
		     "the handle holds rather more than the configuration");
}

/* Tests: rollback detection ----------------------------------------------- */

ZTEST(ubi_integration, test_the_rollback_counters_survive_a_reattach)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	/* Only the two blocks holding the volume table have been stamped, and
	 * they carry the only sequence numbers issued so far. */
	zassert_equal(2, info.healthy_pebs);
	zassert_equal(2, info.total_erase_count);
	zassert_equal(2, info.global_sqnum);
	zassert_equal(1, info.revision);
}

ZTEST(ubi_integration, test_reformatting_does_not_restart_the_sequence_numbers)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	/*
	 * A format leaves most blocks untouched, so if it restarted the
	 * numbering a volume table left by the previous image would outrank
	 * the fresh one and the next attach would quietly undo the format.
	 */
	zassert_true(
		second.global_sqnum > first.global_sqnum,
		"a reformat must outrank whatever is already on the flash");
}

ZTEST(ubi_integration, test_reformatting_keeps_the_wear_history)
{
	struct ubi_device_info first = { 0 };
	struct ubi_device_info second = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &first));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &second));
	zassert_ok(ubi_device_deinit(ubi));

	/* A reformat reads the old erase count back and carries on from it,
	 * so the total never drops: that is what makes it usable as a
	 * rollback anchor. */
	zassert_true(second.total_erase_count > first.total_erase_count);
}

ZTEST(ubi_integration, test_the_state_check_sees_what_get_info_reports)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(1, state_check_count, "an attach asks exactly once");
	zassert_equal(info.image_seq, last_state.image_seq);
	zassert_equal(info.global_sqnum, last_state.global_sqnum);
	zassert_equal(info.total_erase_count, last_state.total_erase_count);
	zassert_equal(info.healthy_pebs, last_state.healthy_pebs);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_untrusted_state_stops_the_attach)
{
	struct ubi_config guarded = config;

	guarded.state_cb = trust_nothing;

	zassert_ok(ubi_device_format(&config));

	const uint32_t before = partition_fingerprint();

	zassert_equal(-EROFS, ubi_device_init(ubi, &guarded));
	zassert_equal(1, state_check_count);

	/* Refusing to trust the flash must not be a reason to change it. */
	zassert_equal(before, partition_fingerprint());

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_deinit(ubi));
}
