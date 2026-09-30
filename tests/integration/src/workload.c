/**
 * \file    workload.c
 * \author  Kamil Kielbasa
 * \brief   Devices set up with work for maintenance: wear to level, and
 *          blocks to bring back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "suite.h"
#include "workload.h"

/* Module defines ---------------------------------------------------------- */

/** Rounds of churn after which the hot pair stands one erase past the
 *  threshold above the cold blocks; the pair takes the erases in turn. */
#define WEAR_OUT_ROUNDS (2 * (CONFIG_UBI_WEAR_LEVELING_THRESHOLD + 1))

/** Updates that leave the volume table copies worn past blocks written once,
 *  so that relocation reaches for those first. */
#define VOLUME_TABLE_AGEING (2)

/* Module interface function definitions ----------------------------------- */

void volume_table_age(void)
{
	const struct ubi_volume_config scratch = { .name = "scratch",
						   .leb_count = 1 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	for (uint32_t i = 0; i < VOLUME_TABLE_AGEING; ++i) {
		zassert_ok(ubi_volume_create(ubi, &scratch, &vol_id));
		zassert_ok(ubi_volume_remove(ubi, vol_id));
	}
}

void wear_out_one_block(uint32_t vol_id)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x8C);

	for (uint32_t round = 0; round < WEAR_OUT_ROUNDS; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
		zassert_equal(1, result.performed);
	}

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(0 < info.relocatable_pebs,
		     "the spread has to grow past the threshold");
}

uint32_t unevenly_worn_device(const uint8_t *cold, size_t length,
			      uint32_t *leb_count)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	volume_table_age();
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 0; lnum < wanted.leb_count; ++lnum)
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, cold, length));

	wear_out_one_block(vol_id);

	/* Next to the worn spare, the block a cold one gives back. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, wanted.leb_count - 1));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining);

	*leb_count = wanted.leb_count;

	return vol_id;
}

uint32_t cold_wear_max(uint32_t vol_id, uint32_t leb_count)
{
	uint32_t worst = 0;

	for (uint32_t lnum = 1; lnum < leb_count; ++lnum)
		worst = MAX(worst, leb_wear(vol_id, lnum));

	return worst;
}

void cold_blocks_check(uint32_t vol_id, uint32_t leb_count, const uint8_t *cold,
		       size_t length)
{
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_equal(sizeof(read), length);

	for (uint32_t lnum = 1; lnum < leb_count - 1; ++lnum) {
		memset(read, 0x00, sizeof(read));
		zassert_ok(
			ubi_leb_read(ubi, vol_id, lnum, 0, read, sizeof(read)));
		zassert_mem_equal(cold, read, sizeof(read),
				  "block %u changed under relocation", lnum);
	}
}

#if defined(CONFIG_FLASH_SIMULATOR)

void retire_one_block(uint32_t vol_id, uint32_t lnum)
{
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x5E);

	flash_fail_writes_after(0);
	zassert_equal(-EIO, ubi_leb_change(ubi, vol_id, lnum, written,
					   sizeof(written)));
	flash_fail_writes_never();
}

#endif /* CONFIG_FLASH_SIMULATOR */
