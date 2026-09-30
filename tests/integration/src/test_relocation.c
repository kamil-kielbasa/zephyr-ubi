/**
 * \file    test_relocation.c
 * \author  Kamil Kielbasa
 * \brief   Moving cold data onto worn blocks, and what it carries along.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "forge.h"
#include "partition.h"
#include "suite.h"
#include "workload.h"

/* Module defines ---------------------------------------------------------- */

/** Blocks reclaimed next to the worn one, for relocation to choose from. */
#define RECLAIM_BUDGET (4)

/** How far above the least worn free block relocation reaches for a target,
 *  as the help of CONFIG_UBI_WEAR_LEVELING_THRESHOLD states. */
#define RELOCATION_REACH (2 * CONFIG_UBI_WEAR_LEVELING_THRESHOLD)

/** Erased bytes cold data ends on, which relocation trims. */
#define ERASED_TAIL (3)

/* Trimming the tail has to land off a write block boundary. */
BUILD_ASSERT(1 == UBI_TEST_WRITE_BLOCK || ERASED_TAIL < UBI_TEST_WRITE_BLOCK);

/** Where a bit error lands in the erased tail of a block written by appends,
 *  past everything the test writes there. */
#define STRAY_OFFSET (3 * UBI_TEST_PAYLOAD_SIZE)

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_relocation);

/* Module interface function definitions ----------------------------------- */

/* Tests: when relocation moves a block ------------------------------------ */

/*
 * Given: an unevenly worn device, with cold data on barely used blocks and
 *        one block the churn has been hammering.
 * When:  one relocation step runs.
 * Then:  cold data ends up on the worn block, nothing is lost or duplicated,
 *        and the move survives a reattach.
 */
ZTEST(ubi_relocation, test_relocation_moves_cold_data_onto_a_worn_block)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0x9D);
	memset(&cold[sizeof(cold) - ERASED_TAIL], UBI_TEST_ERASED, ERASED_TAIL);

	const uint32_t vol_id =
		unevenly_worn_device(cold, sizeof(cold), &leb_count);

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_true(before.max_erase_count - before.min_erase_count >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the device has to be unevenly worn to begin with");
	zassert_equal(before.min_erase_count, cold_wear_max(vol_id, leb_count),
		      "the cold blocks are the least worn on the device");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(1, result.performed);
	zassert_equal(0, result.remaining, "the one worn free block is taken");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_true(cold_wear_max(vol_id, leb_count) >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "cold data has to end up on a block that has seen wear");
	zassert_equal(mapped_pebs(&before), mapped_pebs(&after),
		      "one logical block may occupy only one physical one");
	cold_blocks_check(vol_id, leb_count, cold, sizeof(cold));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_true(cold_wear_max(vol_id, leb_count) >
			     CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the move has to survive a reattach");
	cold_blocks_check(vol_id, leb_count, cold, sizeof(cold));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose blocks are all worn about the same.
 * When:  relocation is offered a budget.
 * Then:  it does nothing, because nothing is worth moving yet.
 */
ZTEST(ubi_relocation, test_an_even_device_has_nothing_to_relocate)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xAE);

	for (uint32_t lnum = 0; lnum < UBI_TEST_VOLUME_LEBS; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));
	}

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(0, result.performed, "nothing is worn out yet");
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: one free block worn exactly as far above the least worn free block
 *        as relocation may reach.
 * When:  relocation is offered a budget.
 * Then:  it does nothing, because that block is out of reach and moving cold
 *        data there would spend the last of it.
 */
ZTEST(ubi_relocation, test_a_block_too_far_gone_is_not_relocated_onto)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xC3);

	for (uint32_t lnum = 0; lnum < UBI_TEST_VOLUME_LEBS; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, written,
					  sizeof(written)));
	}

	/* Free blocks to choose from next to the worn one. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, RECLAIM_BUDGET,
				   &result));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	const uint32_t far_gone = info.min_erase_count + RELOCATION_REACH;

	stamp_erase_count(config.ikm_key_id, info.peb_count - 1, info.image_seq,
			  far_gone);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(far_gone, info.max_erase_count,
		      "the stamped block has to be the worn one");
	zassert_equal(RECLAIM_BUDGET + 1, info.free_pebs);

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));

	zassert_equal(0, result.performed,
		      "the only worn block is out of reach");
	zassert_equal(0, result.remaining);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what a move carries ---------------------------------------------- */

/*
 * Given: a block written by an append, so no header claims how far its data
 *        goes, which relocation has since carried to another block.
 * When:  the caller appends to it again.
 * Then:  the write goes through, because the seal relocation put on it
 *        covers only what was really written.
 */
ZTEST(ubi_relocation, test_relocation_leaves_room_to_append_afterwards)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0xE2);
	pattern_fill(second, sizeof(second), 0xF3);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	volume_table_age();
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	/* The hot block takes the worn one the table moved out of last. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, first, sizeof(first)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, 0, first, sizeof(first)));

	for (uint32_t lnum = 2; lnum < wanted.leb_count; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, first,
					  sizeof(first)));
	}

	wear_out_one_block(vol_id);

	/* Of the equally cold blocks, relocation takes the lowest numbered,
	 * and block 1 was the first one written. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);
	zassert_true(leb_wear(vol_id, 1) > CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the appended block has to have been relocated");

	/* A seal over the erased tail as well would condemn this append at
	 * the next attach. */
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, sizeof(first), second,
				    sizeof(second)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(first, read, sizeof(first));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, sizeof(first), read,
				sizeof(read)));
	zassert_mem_equal(second, read, sizeof(second));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block sealed by a change and then appended to past its seal.
 * When:  relocation moves it, and the device is attached again.
 * Then:  the appended bytes moved with it, because relocation copies the
 *        whole block, not only what the seal covered.
 */
ZTEST(ubi_relocation, test_relocation_carries_what_was_appended_past_the_seal)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t changed[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t appended[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(changed, sizeof(changed), 0x1B);
	pattern_fill(appended, sizeof(appended), 0x2C);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	volume_table_age();
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	/* The hot block takes the worn one the table moved out of last. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, changed, sizeof(changed)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, changed, sizeof(changed)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, sizeof(changed), appended,
				    sizeof(appended)));

	for (uint32_t lnum = 2; lnum < wanted.leb_count; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, changed,
					  sizeof(changed)));
	}

	wear_out_one_block(vol_id);

	/* Block 1 was written first, so it is the lowest numbered cold one. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);
	zassert_true(leb_wear(vol_id, 1) > CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the appended block has to have been relocated");

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, sizeof(changed), read,
				sizeof(read)));
	zassert_mem_equal(appended, read, sizeof(appended),
			  "what was appended past the seal has to move too");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(changed, read, sizeof(changed));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, sizeof(changed), read,
				sizeof(read)));
	zassert_mem_equal(appended, read, sizeof(appended),
			  "and it has to survive a reattach");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block written by appends, whose erased tail took a bit error that
 *        relocation then carried along and sealed over.
 * When:  the caller appends into that tail, inside the seal, and the device
 *        is attached again.
 * Then:  nothing is lost: the block fails its seal and is reported, but with
 *        no older copy to fall back to it is kept as it reads.
 */
ZTEST(ubi_relocation, test_an_append_under_a_stray_bit_loses_nothing)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t filler[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0x3D);
	pattern_fill(second, sizeof(second), 0x4E);
	pattern_fill(filler, sizeof(filler), 0x5F);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	volume_table_age();
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	/* The hot block takes the worn one the table moved out of last. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, filler, sizeof(filler)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, 0, first, sizeof(first)));

	const uint32_t pnum = pnum_of_data_matching(first, sizeof(first));

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	flash_clear_a_bit(flash_area, (off_t)pnum * UBI_TEST_PEB_SIZE +
					      UBI_DATA_OFFSET + STRAY_OFFSET);
	flash_area_close(flash_area);

	for (uint32_t lnum = 2; lnum < wanted.leb_count; ++lnum) {
		zassert_ok(ubi_leb_change(ubi, vol_id, lnum, filler,
					  sizeof(filler)));
	}

	wear_out_one_block(vol_id);

	/* Block 1 is the lowest numbered of the equally cold ones. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);
	zassert_true(leb_wear(vol_id, 1) > CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "the appended block has to have been relocated");

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, sizeof(first), second,
				    sizeof(second)));
	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(1, event_count[UBI_EVENT_DATA_CORRUPT],
		      "the append has to break the seal over the stray bit");
	zassert_equal(1, event_last[UBI_EVENT_DATA_CORRUPT].lnum);

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(first, read, sizeof(first));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, sizeof(first), read,
				sizeof(read)));
	zassert_mem_equal(second, read, sizeof(second),
			  "the append has to survive the reattach");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: cold blocks whose data rotted after they were sealed.
 * When:  relocation moves one, and the device is attached again.
 * Then:  the bytes move as they read and are sealed afresh, so the moved
 *        block reads back what the old one held. The blocks it did not move
 *        are still judged by their old seal at the attach and reported, and
 *        kept, as nothing older stands in.
 */
ZTEST(ubi_relocation, test_relocation_moves_a_block_as_it_reads_it)
{
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t rotted[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0xA7);

	const uint32_t vol_id =
		unevenly_worn_device(cold, sizeof(cold), &leb_count);
	const uint32_t holding = count_data_matching(cold, sizeof(cold));

	zassert_true(1 < holding);
	zassert_equal(holding, corrupt_data_matching(cold, sizeof(cold)));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rotted, sizeof(rotted)));
	zassert_not_equal(0, memcmp(cold, rotted, sizeof(cold)));

	/* Block 1 was written first, so it is the lowest numbered cold one. */
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);
	zassert_true(leb_wear(vol_id, 1) > CONFIG_UBI_WEAR_LEVELING_THRESHOLD,
		     "block 1 has to have been moved");

	zassert_ok(ubi_device_deinit(ubi));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(rotted, read, sizeof(read),
			  "the moved block carries a seal over what it held");
	zassert_equal(holding - 1, event_count[UBI_EVENT_DATA_CORRUPT],
		      "every block left behind fails its own seal");

	for (uint32_t lnum = 2; lnum < leb_count - 1; ++lnum) {
		struct ubi_leb_info leb = { 0 };

		zassert_ok(ubi_leb_get_info(ubi, vol_id, lnum, &leb));
		zassert_true(leb.mapped, "block %u was dropped", lnum);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: cold blocks whose volume identifier headers were damaged while the
 *        device was attached, so relocation cannot vouch for any of them.
 * When:  relocation tries to move one, and repairs are run for everything
 *        pending.
 * Then:  relocation refuses and reports the header; the block is neither
 *        retired nor chosen again, and every block still sits on the flash
 *        backing its logical block: a repair never erases a block that holds
 *        data, nor hands it out a second time.
 */
ZTEST(ubi_relocation, test_a_block_relocation_refuses_stays_where_it_is)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	uint8_t cold[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(cold, sizeof(cold), 0xB8);

	const uint32_t vol_id =
		unevenly_worn_device(cold, sizeof(cold), &leb_count);
	const uint32_t holding = count_data_matching(cold, sizeof(cold));

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_true(0 < holding);
	zassert_equal(holding,
		      corrupt_header_of_data_matching(cold, sizeof(cold)));

	events_forget();

	zassert_equal(-EBADMSG, ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
						1, &result));

	zassert_equal(0, result.performed);
	zassert_equal(1, event_count[UBI_EVENT_HDR_CORRUPT]);
	zassert_equal(0, event_count[UBI_EVENT_PEB_BAD]);

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.bad_pebs, after.bad_pebs,
		      "a block that still backs data is not retired");
	zassert_equal(before.relocatable_pebs - 1, after.relocatable_pebs,
		      "and it is not chosen again");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, after.peb_count,
				   &result));

	zassert_equal(holding, count_data_matching(cold, sizeof(cold)),
		      "a repair must not erase a block that backs data");

	for (uint32_t lnum = 1; lnum < leb_count - 1; ++lnum) {
		struct ubi_leb_info leb = { 0 };

		zassert_ok(ubi_leb_get_info(ubi, vol_id, lnum, &leb));
		zassert_true(leb.mapped, "block %u lost its mapping", lnum);
	}

	zassert_ok(ubi_device_deinit(ubi));
}
