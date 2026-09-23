/**
 * \file    suite.c
 * \author  Kamil Kielbasa
 * \brief   The suites' keys, handle and lifecycle.
 *
 *          Every test starts from a blank partition and a zeroed handle.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

struct ubi_device *ubi = NULL;

struct ubi_config config = { 0 };
struct ubi_config config_wrong_key = { 0 };

uint32_t events_total = 0;
uint32_t event_count[UBI_EVENT_PEB_BAD + 1] = { 0 };
struct ubi_event event_last[UBI_EVENT_PEB_BAD + 1] = { 0 };
uint32_t state_check_count = 0;
struct ubi_device_info last_state = { 0 };

/** Keying material the partition is formatted with. */
static const uint8_t ikm_bytes[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
	0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

static psa_key_id_t key_right = PSA_KEY_ID_NULL;
static psa_key_id_t key_wrong = PSA_KEY_ID_NULL;

/** Whether suite_setup() has already run; every suite calls it. */
static bool suite_is_up = false;

/* Static function declarations -------------------------------------------- */

/**
 * \brief Import keying material as a PSA derivation key.
 */
static psa_key_id_t import_ikm(const uint8_t *bytes, size_t length);

/**
 * \brief Event sink that only records what it was told.
 */
static void on_event(const struct ubi_event *event, void *user_context);

/* Static function definitions --------------------------------------------- */

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

static void on_event(const struct ubi_event *event, void *user_context)
{
	ARG_UNUSED(user_context);

	zassert_true(event->type < ARRAY_SIZE(event_count),
		     "event type %u is not one the library documents",
		     event->type);

	events_total += 1;
	event_count[event->type] += 1;
	event_last[event->type] = *event;
}

/* Module interface function definitions ----------------------------------- */

void *suite_setup(void)
{
	uint8_t other[sizeof(ikm_bytes)] = { 0 };

	if (suite_is_up)
		return NULL;

	zassert_equal(PSA_SUCCESS, psa_crypto_init());

	partition_geometry_check();

	key_right = import_ikm(ikm_bytes, sizeof(ikm_bytes));

	memcpy(other, ikm_bytes, sizeof(other));
	other[0] ^= 0xFF;
	key_wrong = import_ikm(other, sizeof(other));

	config.flash_area_id = UBI_TEST_PARTITION_ID;
	config.ikm_key_id = key_right;
	config.event_cb = on_event;
	config.state_cb = trust_everything;

	config_wrong_key = config;
	config_wrong_key.ikm_key_id = key_wrong;

	suite_is_up = true;

	return NULL;
}

void suite_before(void *fixture)
{
	ARG_UNUSED(fixture);

	events_forget();
	state_check_count = 0;
	last_state = (struct ubi_device_info){ 0 };

	ubi = k_calloc(1, ubi_device_size());
	zassert_not_null(ubi, "no memory for a device handle");

#if defined(CONFIG_FLASH_SIMULATOR)
	flash_fail_writes_never();
	flash_fail_erases_never();
#endif

	partition_erase_dirty();
}

void suite_after(void *fixture)
{
	int ret = 0;

	ARG_UNUSED(fixture);

	/* A test that stopped half-way leaves the handle attached, and its
	 * block tables would starve the next attach of heap. */
	if (UBI_DEVICE_MAGIC == ubi->magic)
		ret = ubi_device_deinit(ubi);

	k_free(ubi);
	ubi = NULL;

	zassert_ok(ret, "the handle left attached would not detach");
}

void events_forget(void)
{
	events_total = 0;
	memset(event_count, 0, sizeof(event_count));
	memset(event_last, 0, sizeof(event_last));
}

enum ubi_state_verdict trust_everything(const struct ubi_device_info *info,
					void *user_context)
{
	ARG_UNUSED(user_context);

	state_check_count += 1;
	last_state = *info;

	return UBI_STATE_TRUSTED;
}

enum ubi_state_verdict trust_nothing(const struct ubi_device_info *info,
				     void *user_context)
{
	ARG_UNUSED(user_context);

	state_check_count += 1;
	last_state = *info;

	return UBI_STATE_UNTRUSTED;
}

enum ubi_state_verdict trust_once(const struct ubi_device_info *info,
				  void *user_context)
{
	ARG_UNUSED(user_context);

	state_check_count += 1;
	last_state = *info;

	/* The attach asks first; refusing every later check reaches the latch. */
	return (1 == state_check_count) ? UBI_STATE_TRUSTED :
					  UBI_STATE_UNTRUSTED;
}

uint32_t volume_ready(uint32_t leb_count)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = leb_count };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	return vol_id;
}

uint32_t mapped_pebs(const struct ubi_device_info *info)
{
	return info->peb_count - info->free_pebs - info->reclaimable_pebs -
	       info->bad_pebs;
}

uint32_t leb_wear(uint32_t vol_id, uint32_t lnum)
{
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_get_info(ubi, vol_id, lnum, &info));

	return info.erase_count;
}

void pattern_fill(uint8_t *buffer, size_t length, uint8_t seed)
{
	for (size_t i = 0; i < length; ++i)
		buffer[i] = (uint8_t)(seed + i);
}

void leb_payload(uint8_t seed, uint32_t lnum, uint8_t *buffer, size_t length)
{
	pattern_fill(buffer, length, (uint8_t)(seed + lnum));
}

uint32_t volume_under_load(uint32_t *vol_id)
{
	struct ubi_volume_config wanted = { .name = "wear", .leb_count = 0 };
	struct ubi_device_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_true(info.free_lebs > UBI_TEST_LOAD_FREE_LEBS);
	wanted.leb_count = info.free_lebs - UBI_TEST_LOAD_FREE_LEBS;

	zassert_ok(ubi_volume_create(ubi, &wanted, vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum) {
		leb_payload(UBI_TEST_LOAD_SEED, lnum, written, sizeof(written));
		zassert_ok(ubi_leb_change(ubi, *vol_id, lnum, written,
					  sizeof(written)));
	}

	return wanted.leb_count;
}

uint32_t pool_ready(uint32_t *vol_id)
{
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	const uint32_t leb_count = volume_under_load(vol_id);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.reclaimable_pebs, &result));
	zassert_equal(0, result.remaining, "the free pool has to be ready");

	return leb_count;
}
