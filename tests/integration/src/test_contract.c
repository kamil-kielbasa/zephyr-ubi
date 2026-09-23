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
#include <zephyr/ztest.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_contract);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: configurations each missing one thing the library cannot do without.
 * When:  a format or an attach is asked for with them.
 * Then:  every one is refused, and refused before the flash is touched.
 */
ZTEST(ubi_contract, test_a_configuration_without_its_parts_is_refused)
{
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
 *        fits is granted and found.
 */
ZTEST(ubi_contract, test_a_name_the_device_cannot_store_is_refused)
{
	static const char too_long[] = "0123456789abcdef";
	static const char longest[] = "0123456789abcde";
	struct ubi_volume_config wanted = { .name = NULL,
					    .leb_count = UBI_TEST_VOLUME_LEBS };
	struct ubi_device_info info = { 0 };
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
 * Given: a formatted partition and a handle nobody attached.
 * When:  each block-level call is made on it.
 * Then:  every one is refused, so a handle that failed to attach cannot be
 *        used by mistake.
 */
ZTEST(ubi_contract, test_a_detached_handle_takes_no_block_calls)
{
	struct ubi_leb_info info = { 0 };
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));

	zassert_equal(-EINVAL, ubi_leb_map(ubi, 0, 0));
	zassert_equal(-EINVAL, ubi_leb_unmap(ubi, 0, 0));
	zassert_equal(-EINVAL, ubi_leb_erase(ubi, 0, 0));
	zassert_equal(-EINVAL, ubi_leb_get_info(ubi, 0, 0, &info));
	zassert_equal(-EINVAL,
		      ubi_leb_read(ubi, 0, 0, 0, buffer, sizeof(buffer)));
	zassert_equal(-EINVAL,
		      ubi_leb_change(ubi, 0, 0, buffer, sizeof(buffer)));
	zassert_equal(-EINVAL,
		      ubi_leb_write_at(ubi, 0, 0, 0, buffer, sizeof(buffer)));
}
