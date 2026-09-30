/**
 * \file    test_contract.c
 * \author  Kamil Kielbasa
 * \brief   What every entry point refuses before it touches the flash.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "partition.h"
#include "suite.h"

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief What a callback got back when it called into its own device.
 */
struct reentry {
	/** Times the callback tried. */
	uint32_t tries;
	/** What ubi_device_get_info() returned. */
	int get_info;
	/** What ubi_device_deinit() returned. */
	int deinit;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Call into the attached device, as a callback must not.
 */
static void reentry_try(void);

/**
 * \brief Trust the attach, and from then on try to call back in.
 */
static enum ubi_state_verdict
trust_and_call_back(const struct ubi_device_info *info, void *user_context);

#if defined(CONFIG_FLASH_SIMULATOR)

/**
 * \brief Count the event and try to call back in.
 */
static void call_back_on_event(const struct ubi_event *event,
			       void *user_context);

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_contract);

static struct reentry reentry = { 0 };

/* Static function definitions --------------------------------------------- */

static void reentry_try(void)
{
	struct ubi_device_info info = { 0 };

	reentry.tries += 1;
	reentry.get_info = ubi_device_get_info(ubi, &info);
	reentry.deinit = ubi_device_deinit(ubi);
}

static enum ubi_state_verdict
trust_and_call_back(const struct ubi_device_info *info, void *user_context)
{
	ARG_UNUSED(info);
	ARG_UNUSED(user_context);

	state_check_count += 1;

	/* The first check is the attach's, before the handle is usable. */
	if (1 < state_check_count)
		reentry_try();

	return UBI_STATE_TRUSTED;
}

#if defined(CONFIG_FLASH_SIMULATOR)

static void call_back_on_event(const struct ubi_event *event,
			       void *user_context)
{
	ARG_UNUSED(user_context);

	event_count[event->type] += 1;
	reentry_try();
}

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module interface function definitions ----------------------------------- */

/*
 * Given: configurations each missing one thing the library cannot do without.
 * When:  a format or an attach is asked for with them.
 * Then:  every one is refused, and refused before the flash is touched.
 */
ZTEST(ubi_contract, test_a_configuration_without_its_parts_is_refused)
{
	static const uint8_t too_long[UBI_KEY_CONTEXT_MAX_SIZE + 1] = { 0 };
	const uint32_t before = partition_fingerprint();
	struct ubi_config broken = config;

	zassert_equal(-EINVAL, ubi_device_format(NULL));
	zassert_equal(-EINVAL, ubi_device_init(ubi, NULL));
	zassert_equal(-EINVAL, ubi_device_init(NULL, &config));

	broken = config;
	broken.ikm_key_id = PSA_KEY_ID_NULL;
	zassert_equal(-EINVAL, ubi_device_format(&broken),
		      "without keying material nothing could be sealed");
	zassert_equal(-EINVAL, ubi_device_init(ubi, &broken));

	broken = config;
	broken.event_cb = NULL;
	zassert_equal(-EINVAL, ubi_device_format(&broken),
		      "damage the application never hears about is worse "
		      "than none");
	zassert_equal(-EINVAL, ubi_device_init(ubi, &broken));

	broken = config;
	broken.state_cb = NULL;
	zassert_equal(-EINVAL, ubi_device_format(&broken),
		      "without a verdict there is nobody to catch a rollback");
	zassert_equal(-EINVAL, ubi_device_init(ubi, &broken));

	broken = config;
	broken.key_context_size = 1;
	zassert_equal(-EINVAL, ubi_device_format(&broken),
		      "a key context of one byte with no bytes behind it");
	zassert_equal(-EINVAL, ubi_device_init(ubi, &broken));

	broken = config;
	broken.key_context = too_long;
	broken.key_context_size = sizeof(too_long);
	zassert_equal(-EINVAL, ubi_device_format(&broken),
		      "a key context longer than any the library takes");
	zassert_equal(-EINVAL, ubi_device_init(ubi, &broken));

	zassert_equal(before, partition_fingerprint(),
		      "a refused argument must not reach the flash");
}

/*
 * Given: an attached device.
 * When:  each call that fills something in is handed nowhere to put it.
 * Then:  every one is refused rather than writing through a null pointer.
 */
ZTEST(ubi_contract, test_a_call_with_nowhere_to_answer_is_refused)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-EINVAL, ubi_device_get_info(ubi, NULL));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, NULL, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, NULL));
	zassert_equal(-EINVAL, ubi_volume_find(ubi, NULL, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_find(ubi, wanted.name, NULL));
	zassert_equal(-EINVAL, ubi_volume_get_info(ubi, vol_id, NULL));
	zassert_equal(-EINVAL, ubi_leb_get_info(ubi, vol_id, 0, NULL));
	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, 0, NULL,
					    UBI_TEST_PAYLOAD_SIZE));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, 0, NULL,
					      UBI_TEST_PAYLOAD_SIZE));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, 0, NULL,
						UBI_TEST_PAYLOAD_SIZE));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an attached device.
 * When:  a volume is asked for under a name the device cannot store.
 * Then:  each is refused and no volume appears, while the longest name that
 *        fits is granted, found, and read back whole after a reattach.
 */
ZTEST(ubi_contract, test_a_name_the_device_cannot_store_is_refused)
{
	static const char too_long[] = "0123456789abcdefg";
	static const char longest[] = "0123456789abcdef";
	struct ubi_volume_config wanted = { .name = NULL,
					    .leb_count = UBI_TEST_VOLUME_LEBS };
	struct ubi_device_info info = { 0 };
	struct ubi_volume_info volume = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;

	BUILD_ASSERT(sizeof(too_long) - 1 == UBI_VOLUME_NAME_MAX_LEN + 1);
	BUILD_ASSERT(sizeof(longest) - 1 == UBI_VOLUME_NAME_MAX_LEN);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	wanted.name = too_long;
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id));

	wanted.name = "";
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &vol_id),
		      "a volume nobody can name is a volume nobody can find");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);

	wanted.name = longest;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_volume_find(ubi, longest, &found));
	zassert_equal(vol_id, found);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_volume_find(ubi, longest, &found));
	zassert_equal(vol_id, found);
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &volume));
	zassert_str_equal(
		longest, volume.name,
		"a name filling the field has no terminator on flash");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device already holding every volume this build allows.
 * When:  one more is asked for.
 * Then:  it is refused for want of room in the table, and the volumes that
 *        are there are untouched.
 */
ZTEST(ubi_contract, test_one_volume_more_than_the_build_allows_is_refused)
{
	char name[UBI_VOLUME_NAME_MAX_LEN + 1] = { 0 };
	struct ubi_volume_config wanted = { .name = name, .leb_count = 1 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	for (uint32_t i = 0; i < CONFIG_UBI_MAX_NR_OF_VOLUMES; ++i) {
		const int length = snprintf(name, sizeof(name), "vol%u", i);

		zassert_between_inclusive(length, 1, (int)sizeof(name) - 1);
		zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id),
			   "volume %u of %d should still fit", i,
			   CONFIG_UBI_MAX_NR_OF_VOLUMES);
	}

	wanted.name = "one too many";
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &wanted, &vol_id),
		      "the table holds %d records and no more",
		      CONFIG_UBI_MAX_NR_OF_VOLUMES);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(CONFIG_UBI_MAX_NR_OF_VOLUMES, info.volume_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an attached device with one volume.
 * When:  the identifier that stands for "no volume" is passed in.
 * Then:  every call refuses it, so a caller who forgot to check an earlier
 *        failure cannot reach somebody else's data with it.
 */
ZTEST(ubi_contract, test_the_invalid_volume_identifier_is_refused)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_volume_info volume = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	zassert_equal(-EINVAL,
		      ubi_volume_get_info(ubi, UBI_VOL_ID_INVALID, &volume));
	zassert_equal(-EINVAL, ubi_volume_remove(ubi, UBI_VOL_ID_INVALID));
	zassert_equal(-EINVAL, ubi_volume_resize(ubi, UBI_VOL_ID_INVALID,
						 UBI_TEST_VOLUME_LEBS));
	zassert_equal(-EINVAL,
		      ubi_leb_get_info(ubi, UBI_VOL_ID_INVALID, 0, &leb));
	zassert_equal(-EINVAL, ubi_leb_map(ubi, UBI_VOL_ID_INVALID, 0));
	zassert_equal(-EINVAL, ubi_leb_unmap(ubi, UBI_VOL_ID_INVALID, 0));
	zassert_equal(-EINVAL, ubi_leb_erase(ubi, UBI_VOL_ID_INVALID, 0));
	zassert_equal(-EINVAL, ubi_leb_read(ubi, UBI_VOL_ID_INVALID, 0, 0,
					    buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, UBI_VOL_ID_INVALID, 0,
					      buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, UBI_VOL_ID_INVALID, 0, 0,
						buffer, sizeof(buffer)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a handle that held a device with a volume and was detached.
 * When:  every call is made on it, with the identifiers it used to know.
 * Then:  every one is refused, a second detach included, so a handle cannot
 *        be used past its detach or before a successful attach.
 */
ZTEST(ubi_contract, test_a_detached_handle_takes_no_calls)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info device = { 0 };
	struct ubi_volume_info volume = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t found = UBI_VOL_ID_INVALID;
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(-EINVAL, ubi_device_get_info(ubi, &device));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &wanted, &found));
	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, 1));
	zassert_equal(-EINVAL, ubi_volume_remove(ubi, vol_id));
	zassert_equal(-EINVAL, ubi_volume_find(ubi, wanted.name, &found));
	zassert_equal(-EINVAL, ubi_volume_get_info(ubi, vol_id, &volume));
	zassert_equal(-EINVAL, ubi_leb_map(ubi, vol_id, 0));
	zassert_equal(-EINVAL, ubi_leb_unmap(ubi, vol_id, 0));
	zassert_equal(-EINVAL, ubi_leb_erase(ubi, vol_id, 0));
	zassert_equal(-EINVAL,
		      ubi_leb_read(ubi, vol_id, 0, 0, buffer, sizeof(buffer)));
	zassert_equal(-EINVAL,
		      ubi_leb_change(ubi, vol_id, 0, buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, 0, buffer,
						sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_get_info(ubi, vol_id, 0, &leb));
	zassert_equal(-EINVAL, ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					       &result));
	zassert_equal(-EINVAL, ubi_device_deinit(ubi));
}

/*
 * Given: a partition attached through one handle.
 * When:  the same handle attaches again, a second handle attaches, or the
 *        partition is formatted.
 * Then:  the first is refused rather than leaking the attachment, the others
 *        as busy, since each would change the flash under the first handle's
 *        tables; once it detaches, the second attaches.
 */
ZTEST(ubi_contract, test_a_partition_takes_one_handle_at_a_time)
{
	struct ubi_device *second = k_calloc(1, ubi_device_size());

	zassert_not_null(second, "no memory for a second handle");

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(-EBUSY, ubi_device_init(ubi, &config));
	zassert_equal(-EBUSY, ubi_device_init(second, &config));
	zassert_equal(-EBUSY, ubi_device_format(&config));

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(second, &config));
	zassert_ok(ubi_device_deinit(second));

	k_free(second);
}

/*
 * Given: a state callback that calls back into its own device.
 * When:  the interval brings the check back while a write holds the lock.
 * Then:  both calls are refused as a deadlock instead of blocking or
 *        detaching the handle under the write, and the handle carries on.
 */
ZTEST(ubi_contract, test_a_state_callback_cannot_call_back_in)
{
	struct ubi_config watched = config;
	struct ubi_device_info info = { 0 };
	uint8_t record[UBI_TEST_WRITE_BLOCK] = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};

	watched.state_cb = trust_and_call_back;
	reentry = (struct reentry){ 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &watched));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t appended = 0; 0 == reentry.tries; ++appended) {
		zassert_true(appended <= CONFIG_UBI_STATE_CHECK_INTERVAL);
		zassert_ok(ubi_leb_write_at(ubi, vol_id, 0,
					    appended * sizeof(record), record,
					    sizeof(record)));
	}

	zassert_equal(-EDEADLK, reentry.get_info);
	zassert_equal(-EDEADLK, reentry.deinit);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));
}

#if defined(CONFIG_FLASH_SIMULATOR)

/*
 * Given: an event callback that calls back into its own device.
 * When:  a write fails and the block is reported bad.
 * Then:  both calls are refused as a deadlock, and the handle carries on.
 */
ZTEST(ubi_contract, test_an_event_callback_cannot_call_back_in)
{
	struct ubi_config watched = config;
	struct ubi_device_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};

	watched.event_cb = call_back_on_event;
	reentry = (struct reentry){ 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &watched));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	flash_fail_writes_after(0);
	zassert_equal(-EIO,
		      ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	flash_fail_writes_never();

	zassert_equal(1, event_count[UBI_EVENT_PEB_BAD]);
	zassert_equal(1, reentry.tries);
	zassert_equal(-EDEADLK, reentry.get_info);
	zassert_equal(-EDEADLK, reentry.deinit);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block appended to.
 * When:  the caller appends to the same bytes again, which ubi_leb_write_at()
 *        forbids.
 * Then:  the flash takes it without a word, and the harness counts every
 *        byte of it: this is how a test notices the library breaking the
 *        same rule.
 */
ZTEST(ubi_contract, test_writing_the_same_bytes_twice_is_noticed)
{
	uint8_t record[UBI_TEST_WRITE_BLOCK] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, record, sizeof(record)));
	zassert_equal(0, flash_double_writes());

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, record, sizeof(record)));
	zassert_equal(sizeof(record), flash_double_writes());
	zassert_equal(UBI_DATA_OFFSET % UBI_TEST_PEB_SIZE,
		      flash_double_write_first() % UBI_TEST_PEB_SIZE);

	/* The caller broke the rule here, not the library. */
	flash_double_writes_forget();

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_FLASH_SIMULATOR */
