/**
 * \file    test_leb.c
 * \author  Kamil Kielbasa
 * \brief   Mapping, reading and writing logical erase blocks.
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
#include "common.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_leb);

/* What a part accepts is the build's to decide, so it selects the tests. */
UBI_TEST_SUITE(ubi_alignment);

/* Module interface function definitions ----------------------------------- */

/* Tests: the mapping ------------------------------------------------------ */

/*
 * Given: a fresh volume that has mapped nothing.
 * When:  a logical block nobody wrote is read.
 * Then:  it reads as erased, because nothing behind it reads the same as a
 *        block nobody has written.
 */
ZTEST(ubi_leb, test_an_unmapped_block_reads_as_erased)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "a fresh volume maps nothing");
	zassert_equal(0, info.erase_count);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a fresh volume.
 * When:  one of its logical blocks is mapped.
 * Then:  a physical block backs it, the mapping cost a sequence number, and
 *        what it holds reads as erased.
 */
ZTEST(ubi_leb, test_mapping_gives_an_empty_block)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_leb_info info = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_leb_map(ubi, vol_id, 1));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.global_sqnum + 1, after.global_sqnum,
		      "mapping seals one header, so it spends one sequence "
		      "number");

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &info));
	zassert_true(info.mapped);

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a logical block that is already mapped.
 * When:  it is mapped a second time.
 * Then:  the library refuses and writes nothing, because mapping is a
 *        transition rather than a state to assert.
 */
ZTEST(ubi_leb, test_mapping_a_mapped_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	/* Linux answers -EBADMSG, which here means a MAC that failed. */
	zassert_equal(-EEXIST, ubi_leb_map(ubi, vol_id, 0));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.global_sqnum, after.global_sqnum,
		      "a refusal must not write anything");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a mapped logical block.
 * When:  it is unmapped, twice.
 * Then:  the physical block joins the reclaim queue, the mapping is gone, and
 *        asking again is not an error.
 */
ZTEST(ubi_leb, test_unmapping_queues_the_block_for_reclaim)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.reclaimable_pebs + 1, after.reclaimable_pebs);

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume with one block mapped and another left alone.
 * When:  the device is detached and attached again.
 * Then:  the mapping is rebuilt from the flash, and the block nobody mapped
 *        stays unmapped.
 */
ZTEST(ubi_leb, test_a_mapping_survives_a_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 2));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 2, &info));
	zassert_true(info.mapped, "the mapping has to be rebuilt from flash");

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: writing through the mapping -------------------------------------- */

/*
 * Given: a logical block written with a whole-block change.
 * When:  the device is detached and attached again.
 * Then:  the block reads back exactly what was written.
 */
ZTEST(ubi_leb, test_a_change_is_readable_after_a_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x11);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));

	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a logical block that has been changed once already.
 * When:  it is changed again.
 * Then:  it still occupies one physical block and reads the new contents,
 *        while the old block stays on the flash until reclaim takes it.
 */
ZTEST(ubi_leb, test_a_change_leaves_the_old_block_until_reclaim)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x22);
	pattern_fill(fresh, sizeof(fresh), 0x33);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(mapped_pebs(&before), mapped_pebs(&after),
		      "one logical block may occupy only one physical one");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh),
			  "the LEB has to show the new contents");

	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, count_data_matching(fresh, sizeof(fresh)));
	zassert_equal(1, count_data_matching(old, sizeof(old)),
		      "the old block is queued, not erased");
}

/*
 * Given: a logical block holding data.
 * When:  a change and an append are both asked for with a length of zero.
 * Then:  nothing is written and the contents stay, because a degenerate
 *        length must not spend an erase.
 */
ZTEST(ubi_leb, test_writing_nothing_does_nothing)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x44);
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, NULL, 0));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, NULL, 0));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.global_sqnum, after.global_sqnum);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written),
			  "the contents had to stay exactly as they were");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an unmapped logical block.
 * When:  it is appended to without being mapped first.
 * Then:  the write takes a block on its own, as ubi_eba_write_leb() does in
 *        Linux, and a second record placed where the caller says survives a
 *        reattach.
 */
ZTEST(ubi_leb, test_an_append_maps_the_block_it_needs)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x55);

	zassert_ok(
		ubi_leb_write_at(ubi, vol_id, 0, 0, written, sizeof(written)));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_true(info.mapped);

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, sizeof(written), written,
				    sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, sizeof(written), read,
				sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a mapped logical block.
 * When:  a read, an append or a change is asked for past the end of it.
 * Then:  each is refused, while exactly the whole block still goes through.
 */
ZTEST(ubi_leb, test_a_range_past_the_end_of_a_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info device = { 0 };
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(buffer, sizeof(buffer), 0x66);
	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, device.leb_size - 1,
					    buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, device.leb_size,
						buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, 0, buffer,
					      device.leb_size + 1));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0,
				device.leb_size - sizeof(buffer), buffer,
				sizeof(buffer)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume, and the first logical block number past its end.
 * When:  that block, or a volume that does not exist, is named.
 * Then:  the block number is refused as an argument and the volume as a
 *        missing one, so the two mistakes stay distinguishable.
 */
ZTEST(ubi_leb, test_a_block_the_volume_does_not_reach_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const uint32_t past_end = UBI_TEST_VOLUME_LEBS;
	struct ubi_leb_info info = { 0 };
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(buffer, sizeof(buffer), 0x77);

	zassert_equal(-EINVAL, ubi_leb_map(ubi, vol_id, past_end));
	zassert_equal(-EINVAL, ubi_leb_unmap(ubi, vol_id, past_end));
	zassert_equal(-EINVAL, ubi_leb_get_info(ubi, vol_id, past_end, &info));
	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, past_end, 0, buffer,
					    sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, past_end, buffer,
					      sizeof(buffer)));

	zassert_equal(-ENOENT, ubi_leb_map(ubi, vol_id + 1, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what a power loss may leave behind -------------------------------- */

/*
 * Given: a block changed twice, with nothing reclaimed in between, so two
 *        physical blocks carry a sealed header naming the same logical one.
 * When:  the device is attached.
 * Then:  the copy with the higher sequence number wins and the older one is
 *        queued for reclaim, which is how an interrupted reclaim is survived.
 */
ZTEST(ubi_leb, test_the_newer_of_two_copies_wins_the_block)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x6A);
	pattern_fill(fresh, sizeof(fresh), 0x7B);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, count_data_matching(old, sizeof(old)),
		      "the older copy is still on the flash");
	zassert_equal(1, count_data_matching(fresh, sizeof(fresh)));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh),
			  "the higher sequence number has to win");

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.reclaimable_pebs, after.reclaimable_pebs,
		      "the loser is queued for reclaim, exactly as it was "
		      "before the reboot");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block changed twice, with the second write cut short so its data
 *        no longer matches the checksum its sealed header promised.
 * When:  the device is attached.
 * Then:  the older copy wins, so a cut-short write never replaces what was
 *        already there.
 */
ZTEST(ubi_leb, test_an_interrupted_change_leaves_the_old_contents)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x88);
	pattern_fill(fresh, sizeof(fresh), 0x99);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(old, read, sizeof(old),
			  "a cut-short write must not replace what was there");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block written once, with that write cut short.
 * When:  the device is attached.
 * Then:  there was no older copy to fall back to, so the block reads as one
 *        nobody ever wrote.
 */
ZTEST(ubi_leb, test_an_interrupted_first_change_leaves_nothing)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(fresh, sizeof(fresh), 0xA5);
	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block appended to, so no header promises anything about the bytes.
 * When:  the data is damaged and the device attached.
 * Then:  UBI hands back exactly what is on the flash, because an append is
 *        the caller's to check.
 */
ZTEST(ubi_leb, test_an_interrupted_append_is_the_callers_problem)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xC3);

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(
		ubi_leb_write_at(ubi, vol_id, 0, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_not_equal(0, memcmp(written, read, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: getting rid of a block ------------------------------------------- */

/*
 * Given: a block that was written and then unmapped.
 * When:  the device is detached and attached again.
 * Then:  the mapping comes back, because unmapping never reached the flash;
 *        ubi_leb_erase() is the durable way, exactly as in Linux UBI.
 */
ZTEST(ubi_leb, test_an_unmap_may_not_survive_a_reboot)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x1D);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_true(info.mapped, "unmap alone does not reach the flash");
	zassert_equal(1, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding data.
 * When:  it is erased.
 * Then:  the contents are gone from the flash itself and stay gone across a
 *        reboot.
 */
ZTEST(ubi_leb, test_an_erase_takes_the_contents_off_the_flash)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x2E);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_equal(1, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);
	zassert_equal(0, count_data_matching(written, sizeof(written)),
		      "the contents have to be gone from the flash itself");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "and it has to stay gone across a reboot");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding data.
 * When:  it is erased, twice.
 * Then:  it is allocatable again without another erase, the wear history
 *        records it, and asking again is not an error.
 */
ZTEST(ubi_leb, test_an_erase_returns_the_block_to_the_free_pool)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x3F);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.free_pebs + 1, after.free_pebs,
		      "an erased block is allocatable without another erase");
	zassert_equal(before.total_erase_count + 1, after.total_erase_count,
		      "the wear history records the erase");

	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what the write block size imposes -------------------------------- */

#if UBI_TEST_WRITE_BLOCK > 1

/*
 * Given: a flash whose writes must be whole blocks.
 * When:  a change or an append is asked for with a length that is not.
 * Then:  both are refused, and a whole number of write blocks still goes
 *        through and survives a reattach.
 */
ZTEST(ubi_alignment, test_a_length_that_is_not_a_write_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xD1);

	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, 0, written,
					      sizeof(written) - 1));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 1, 0, written,
						sizeof(written) - 1));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a flash whose writes must be whole blocks.
 * When:  an append is asked for at an offset that is not one.
 * Then:  it is refused, because the offset decides where the next write may
 *        start; an aligned offset goes through.
 */
ZTEST(ubi_alignment, test_an_offset_that_is_not_a_write_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xE2);

	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, 1, written,
						UBI_TEST_WRITE_BLOCK));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, UBI_TEST_WRITE_BLOCK,
				    written, UBI_TEST_WRITE_BLOCK));

	zassert_ok(ubi_device_deinit(ubi));
}

#else /* UBI_TEST_WRITE_BLOCK > 1 */

/*
 * Given: a flash that takes a single byte at a time.
 * When:  a change and an append are asked for with an odd length and offset.
 * Then:  both go through, because there is no alignment for such a part to
 *        refuse.
 */
ZTEST(ubi_alignment, test_a_byte_addressable_flash_refuses_no_length)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xF3);

	zassert_ok(
		ubi_leb_change(ubi, vol_id, 0, written, sizeof(written) - 1));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, 1, written,
				    sizeof(written) - 1));

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* UBI_TEST_WRITE_BLOCK > 1 */

/* Tests: appending -------------------------------------------------------- */

/*
 * Given: a fresh volume.
 * When:  one of its blocks is appended to, record by record, up to its last
 *        byte.
 * Then:  nothing more fits: an append at the end, or one straddling it, is
 *        refused and leaves the flash as it was, and every record reads back
 *        after a reattach.
 */
ZTEST(ubi_leb, test_appends_fill_a_block_to_its_last_byte)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info device = { 0 };
	uint8_t record[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint32_t before = 0;
	uint32_t offset = 0;
	uint8_t seed = 0;

	zassert_ok(ubi_device_get_info(ubi, &device));

	while (offset < device.leb_size) {
		const uint32_t length =
			MIN(UBI_TEST_PAYLOAD_SIZE, device.leb_size - offset);

		pattern_fill(record, length, seed);
		zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, offset, record,
					    length));
		offset += length;
		seed++;
	}

	before = partition_fingerprint();

	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, device.leb_size,
						record, UBI_TEST_WRITE_BLOCK));
	zassert_equal(-EINVAL,
		      ubi_leb_write_at(ubi, vol_id, 0,
				       device.leb_size - UBI_TEST_WRITE_BLOCK,
				       record, 2 * UBI_TEST_WRITE_BLOCK));

	zassert_equal(before, partition_fingerprint(),
		      "a refused append must not reach the flash");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	offset = 0;
	seed = 0;

	while (offset < device.leb_size) {
		const uint32_t length =
			MIN(UBI_TEST_PAYLOAD_SIZE, device.leb_size - offset);

		pattern_fill(record, length, seed);
		zassert_ok(ubi_leb_read(ubi, vol_id, 0, offset, read, length));
		zassert_mem_equal(record, read, length,
				  "the record at offset %u", offset);
		offset += length;
		seed++;
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding two appended records.
 * When:  the device is detached and attached again, and a third record is
 *        appended right after them.
 * Then:  all three read back after another reattach: the attach hands the
 *        block back as it was, and the caller's offset still holds.
 */
ZTEST(ubi_leb, test_appending_carries_on_after_a_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t third[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0x6C);
	pattern_fill(second, sizeof(second), 0x7D);
	pattern_fill(third, sizeof(third), 0x8E);

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, first, sizeof(first)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE,
				    second, sizeof(second)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 2 * UBI_TEST_PAYLOAD_SIZE,
				    third, sizeof(third)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(first, read, sizeof(first));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(second, read, sizeof(second));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 2 * UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(third, read, sizeof(third));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding two appended records.
 * When:  it is unmapped, and a new record is appended at offset 0.
 * Then:  the new record starts a fresh block: the rest of it reads as erased
 *        while the old block waits on the flash for reclaim, and the fresh
 *        block still wins after a reattach.
 */
ZTEST(ubi_leb, test_after_an_unmap_an_append_starts_on_a_fresh_block)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0x9F);
	pattern_fill(second, sizeof(second), 0xB0);
	pattern_fill(fresh, sizeof(fresh), 0xC1);
	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, first, sizeof(first)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE,
				    second, sizeof(second)));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, fresh, sizeof(fresh)));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read),
			  "the second record stayed with the old block");

	zassert_equal(1, count_data_matching(first, sizeof(first)),
		      "the old block is queued, not erased");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh),
			  "the fresh block has to win the attach");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block changed and then appended to past what the change claimed.
 * When:  the device is attached.
 * Then:  both regions read back, because the seal covers only the bytes the
 *        change really wrote.
 */
ZTEST(ubi_leb, test_an_append_after_a_change_survives_the_attach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t changed[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t appended[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(changed, sizeof(changed), 0x4A);
	pattern_fill(appended, sizeof(appended), 0x5B);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, changed, sizeof(changed)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, sizeof(changed), appended,
				    sizeof(appended)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(changed, read, sizeof(changed));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, sizeof(changed), read,
				sizeof(read)));
	zassert_mem_equal(appended, read, sizeof(appended));

	zassert_ok(ubi_device_deinit(ubi));
}
