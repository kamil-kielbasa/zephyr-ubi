/**
 * \file    suite.c
 * \author  Kamil Kielbasa
 * \brief   The integration suite: its keys, its handle and its lifecycle.
 *
 *          Every test starts from a blank partition and a zeroed handle, and
 *          none of them has to say so.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
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

struct ubi_device *ubi;

struct ubi_config config;
struct ubi_config config_wrong_key;

uint32_t event_count;
uint32_t event_seen[UBI_EVENT_PEB_BAD + 1];
struct ubi_event event_last[UBI_EVENT_PEB_BAD + 1];
uint32_t state_check_count;
struct ubi_device_info last_state;

static const uint8_t ikm_bytes[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
	0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

static psa_key_id_t key_right;
static psa_key_id_t key_wrong;

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

	event_count += 1;
	event_seen[event->type] += 1;
	event_last[event->type] = *event;
}

void events_forget(void)
{
	event_count = 0;
	memset(event_seen, 0, sizeof(event_seen));
	memset(event_last, 0, sizeof(event_last));
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

	events_forget();
	state_check_count = 0;
	memset(&last_state, 0, sizeof(last_state));

	ubi = k_malloc(ubi_device_size());
	zassert_not_null(ubi, "no memory for a device handle");
	memset(ubi, 0, ubi_device_size());

	flash_fail_writes_never();
	partition_fill(0xFF);
}

static void after_each(void *fixture)
{
	ARG_UNUSED(fixture);

	k_free(ubi);
	ubi = NULL;
}

ZTEST_SUITE(ubi_integration, NULL, suite_setup, before_each, after_each, NULL);

/* Module interface function definitions ----------------------------------- */

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

	/* The attach asks first; everything after it is refused, which is how
	 * a test reaches the latch without unplugging the flash. */
	return (1 == state_check_count) ? UBI_STATE_TRUSTED :
					  UBI_STATE_UNTRUSTED;
}
