/**
 * \file    test_cost.c
 * \author  Kamil Kielbasa
 * \brief   What an operation costs the flash underneath it.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_cost);

/* What a read costs is a build switch, so the build selects the test. */
UBI_TEST_SUITE(ubi_cost_reads);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a device with a volume holding data on most of its blocks.
 * When:  it is attached.
 * Then:  attach reads the headers and the data each sealed block promised,
 *        and writes or erases nothing at all.
 */
ZTEST(ubi_cost, test_attach_reads_the_blocks_and_writes_nothing)
{
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	const uint32_t leb_count = volume_under_load(&vol_id);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	/* Every header, every volume table record, the headers of this image
	 * a second time, and every sealed payload. */
	const uint32_t reads = info.peb_count + UBI_VOLUME_TABLE_LEB_COUNT +
			       mapped_pebs(&info) + info.free_pebs +
			       leb_count * DIV_ROUND_UP(UBI_TEST_PAYLOAD_SIZE,
							DATA_VERIFY_CHUNK);

	flash_ops_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(reads, flash_ops("flash_read_calls"));
	zassert_equal(0, flash_ops("flash_write_calls"));
	zassert_equal(0, flash_ops("flash_erase_calls"));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose free pool has already been erased and stamped.
 * When:  a logical block is rewritten.
 * Then:  the write costs one sealed header and one payload, and no erase:
 *        that is maintenance's to pay, not the writer's.
 */
ZTEST(ubi_cost, test_a_change_on_a_ready_device_costs_no_erase)
{
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x5C);

	zassert_true(0 < pool_ready(&vol_id));

	flash_ops_forget();
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	zassert_equal(0, flash_ops("flash_erase_calls"),
		      "the erase is maintenance's to pay, not the writer's");
	zassert_equal(2, flash_ops("flash_write_calls"),
		      "one sealed header and one payload");
	zassert_equal(0, flash_ops("flash_read_calls"),
		      "where a logical block lives is known in memory");

	zassert_ok(ubi_device_deinit(ubi));
}

#if defined(CONFIG_UBI_VERIFY_ON_READ)

/*
 * Given: a build that re-reads a block's seal before handing bytes back.
 * When:  a logical block is read.
 * Then:  it costs one read more than an unverified one, and nothing else:
 *        checking the seal is the only thing that may add a read.
 */
ZTEST(ubi_cost_reads, test_a_verified_read_costs_one_extra_read)
{
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_true(0 < pool_ready(&vol_id));

	flash_ops_forget();
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));

	zassert_equal(2, flash_ops("flash_read_calls"));
	zassert_equal(0, flash_ops("flash_write_calls"));
	zassert_equal(0, flash_ops("flash_erase_calls"));

	zassert_ok(ubi_device_deinit(ubi));
}

#else /* CONFIG_UBI_VERIFY_ON_READ */

/*
 * Given: a build that takes a read at its word.
 * When:  a logical block is read.
 * Then:  it costs exactly one read and nothing else, because where the block
 *        lives is already known in memory.
 */
ZTEST(ubi_cost_reads, test_a_read_goes_straight_to_the_block)
{
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_true(0 < pool_ready(&vol_id));

	flash_ops_forget();
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));

	zassert_equal(1, flash_ops("flash_read_calls"));
	zassert_equal(0, flash_ops("flash_write_calls"));
	zassert_equal(0, flash_ops("flash_erase_calls"));

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_UBI_VERIFY_ON_READ */

/*
 * Given: a device with one block released and waiting to be reclaimed.
 * When:  a single reclaim step runs.
 * Then:  it costs one erase and one header, and nothing more goes back down.
 */
ZTEST(ubi_cost, test_reclaiming_costs_one_erase_and_one_header)
{
	struct ubi_maintenance_result result = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x6D);

	zassert_true(0 < pool_ready(&vol_id));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	flash_ops_forget();
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));

	zassert_equal(1, result.performed);
	zassert_equal(1, flash_ops("flash_erase_calls"));
	zassert_equal(1, flash_ops("flash_write_calls"),
		      "the erase counter header is all that goes back down");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device that lost one of its two volume table copies outright.
 * When:  a repair replaces it.
 * Then:  it costs one erase, because the replacement comes off the free pool
 *        already erased and a commit must not erase what it was handed.
 */
ZTEST(ubi_cost, test_replacing_a_lost_volume_table_copy_costs_one_erase)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	/* A format puts the copies in the first blocks; restamping the last
	 * of them wipes it, headers and all. */
	stamp_erase_count(config.ikm_key_id, UBI_VOLUME_TABLE_LEB_COUNT - 1,
			  info.image_seq, info.max_erase_count);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "a copy is missing, so the pair is degraded");

	flash_ops_forget();
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);

	zassert_equal(1, flash_ops("flash_erase_calls"),
		      "a commit must not erase a block it was handed erased");

	zassert_ok(ubi_device_deinit(ubi));
}
