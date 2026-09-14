/**
 * \file    test_attach.c
 * \author  Kamil Kielbasa
 * \brief   Recognising what is on the flash, and leaving it alone.
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
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module interface function definitions ----------------------------------- */

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
