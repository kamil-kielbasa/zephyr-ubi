/**
 * \file    test_leb_copies.c
 * \author  Kamil Kielbasa
 * \brief   Which copy of a logical block an attach, a reclaim or an erase
 *          leaves standing.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "partition.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_leb_copies);

/* Module interface function definitions ----------------------------------- */

/* Tests: two copies of one block ------------------------------------------ */

/*
 * Given: a logical block that has been changed once already.
 * When:  it is changed again.
 * Then:  it still occupies one physical block and reads the new contents,
 *        while the old block stays on the flash until reclaim takes it.
 */
ZTEST(ubi_leb_copies, test_a_change_leaves_the_old_block_until_reclaim)
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
 * Given: a block changed twice, with nothing reclaimed in between, so two
 *        physical blocks carry a sealed header naming the same logical one.
 * When:  the device is attached.
 * Then:  the copy with the higher sequence number wins and the older one is
 *        queued for reclaim, which is how an interrupted reclaim is survived.
 */
ZTEST(ubi_leb_copies, test_the_newer_of_two_copies_wins_the_block)
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
ZTEST(ubi_leb_copies, test_an_interrupted_change_leaves_the_old_contents)
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

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(1, event_count[UBI_EVENT_DATA_CORRUPT],
		      "the copy the checksum threw out has to be reported");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(old, read, sizeof(old),
			  "a cut-short write must not replace what was there");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block changed twice, the second copy sitting below the first in
 *        the partition and no longer matching its seal, so that an attach
 *        meets it first.
 * When:  the device is attached.
 * Then:  the older copy still wins: which of the two the attach meets first
 *        makes no difference.
 */
ZTEST(ubi_leb_copies,
      test_an_interrupted_change_met_first_leaves_the_old_contents)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t spacer[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(spacer, sizeof(spacer), 0x6B);
	pattern_fill(old, sizeof(old), 0x7C);
	pattern_fill(fresh, sizeof(fresh), 0x8D);

	/* The spacer holds the free block the table left while the old copy
	 * goes above it, then hands it on to the new one. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, spacer, sizeof(spacer)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 1));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_true(pnum_of_data_matching(fresh, sizeof(fresh)) <
			     pnum_of_data_matching(old, sizeof(old)),
		     "the attach has to meet the newer copy first");
	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(1, event_count[UBI_EVENT_DATA_CORRUPT]);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(
		old, read, sizeof(old),
		"a cut-short write met first replaced what was there");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block written once, whose data no longer matches its seal, as a
 *        write cut short or damage since leaves it.
 * When:  the device is attached.
 * Then:  with no older copy to fall back to, the block is reported and kept
 *        as it reads: dropping it would lose every byte that did reach the
 *        flash.
 */
ZTEST(ubi_leb_copies, test_a_lone_copy_that_fails_its_seal_is_kept)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint32_t differing = 0;

	pattern_fill(fresh, sizeof(fresh), 0xA5);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	const uint32_t pnum = pnum_of_data_matching(fresh, sizeof(fresh));

	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(1, event_count[UBI_EVENT_DATA_CORRUPT],
		      "a block that fails its seal has to be reported");
	zassert_equal(pnum, event_last[UBI_EVENT_DATA_CORRUPT].pnum);

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_true(info.mapped, "the only copy there is has to be kept");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));

	for (size_t i = 0; i < sizeof(read); ++i) {
		if (fresh[i] != read[i])
			differing += 1;
	}

	zassert_equal(1, differing, "the block has to read as it is");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block appended to, so no header promises anything about the bytes.
 * When:  the data is damaged and the device attached.
 * Then:  UBI hands back exactly what is on the flash, because an append is
 *        the caller's to check.
 */
ZTEST(ubi_leb_copies, test_an_interrupted_append_is_the_callers_problem)
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

	events_forget();
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(0, events_total, "no seal covers appended bytes");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_not_equal(0, memcmp(written, read, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: getting rid of a block ------------------------------------------- */

/*
 * Given: a block that was written and then unmapped.
 * When:  the device is detached and attached again.
 * Then:  the mapping comes back, because unmapping never reached the flash;
 *        ubi_leb_erase() is the durable way.
 */
ZTEST(ubi_leb_copies, test_an_unmap_may_not_survive_a_reboot)
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
 * Given: a block changed twice and then unmapped, so both its copies wait
 *        for reclaim, the newer one below the older in the partition.
 * When:  a reclaim step runs, and the device is attached again.
 * Then:  the block comes back with its last contents or with nothing, never
 *        with the ones a change replaced.
 */
ZTEST(ubi_leb_copies, test_an_unmapped_block_never_comes_back_older)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_maintenance_result result = { 0 };
	struct ubi_leb_info info = { 0 };
	uint8_t spacer[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(spacer, sizeof(spacer), 0x7B);
	pattern_fill(old, sizeof(old), 0x8C);
	pattern_fill(fresh, sizeof(fresh), 0x9D);

	/* The spacer holds the free block the table left while the old copy
	 * goes above it, then hands it on to the new one. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, spacer, sizeof(spacer)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 1));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_true(pnum_of_data_matching(fresh, sizeof(fresh)) <
			     pnum_of_data_matching(old, sizeof(old)),
		     "a reclaim has to reach the newer copy first");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(1, result.performed);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));

	if (info.mapped) {
		zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
		zassert_mem_equal(fresh, read, sizeof(fresh),
				  "contents a change replaced came back");
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block unmapped, written again and unmapped again, so both its
 *        copies wait for reclaim as blocks an unmap let go of, the newer one
 *        below the older in the partition.
 * When:  a reclaim step runs, and the device is attached again.
 * Then:  the block does not come back with the contents it had first.
 */
ZTEST(ubi_leb_copies, test_a_block_unmapped_twice_never_comes_back_older)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_maintenance_result result = { 0 };
	struct ubi_leb_info info = { 0 };
	uint8_t spacer[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(spacer, sizeof(spacer), 0xAE);
	pattern_fill(old, sizeof(old), 0xBF);
	pattern_fill(fresh, sizeof(fresh), 0xC1);

	zassert_ok(ubi_leb_change(ubi, vol_id, 1, spacer, sizeof(spacer)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 1));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_true(pnum_of_data_matching(fresh, sizeof(fresh)) <
			     pnum_of_data_matching(old, sizeof(old)),
		     "a reclaim has to reach the newer copy first");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(1, result.performed);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));

	if (info.mapped) {
		zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
		zassert_mem_equal(fresh, read, sizeof(fresh),
				  "the contents it had first came back");
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding data.
 * When:  it is erased, twice.
 * Then:  it is allocatable again without another erase, the wear history
 *        records it, and asking again is not an error.
 */
ZTEST(ubi_leb_copies, test_an_erase_returns_the_block_to_the_free_pool)
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

/*
 * Given: a block changed twice, so its older copy still waits for reclaim.
 * When:  it is erased and the device attached again.
 * Then:  the block is unmapped at once, neither copy is left on the flash,
 *        and the block stays unmapped: an erase that took only the newest
 *        copy would let the reboot bring the older one back.
 */
ZTEST(ubi_leb_copies, test_an_erase_takes_every_copy_with_it)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x4E);
	pattern_fill(fresh, sizeof(fresh), 0x5F);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);
	zassert_equal(0, count_data_matching(fresh, sizeof(fresh)),
		      "the contents have to be gone from the flash itself");
	zassert_equal(0, count_data_matching(old, sizeof(old)),
		      "the older copy has to go as well");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "no copy may come back after a reboot");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block that was written and then unmapped, so its copy waits for
 *        reclaim.
 * When:  it is erased and the device attached again.
 * Then:  the copy is gone and the block stays unmapped: erasing is what
 *        makes an unmap durable, whether the block is mapped or not.
 */
ZTEST(ubi_leb_copies, test_an_erase_makes_an_earlier_unmap_durable)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x6A);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));

	zassert_equal(0, count_data_matching(written, sizeof(written)),
		      "the copy the unmap left has to be gone");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "the unmapped copy came back");

	zassert_ok(ubi_device_deinit(ubi));
}
