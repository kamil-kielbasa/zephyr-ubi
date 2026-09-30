/**
 * \file    test_power_loss_leb.c
 * \author  Kamil Kielbasa
 * \brief   Every point at which a change, an erase or a relocation of a
 *          logical block can fail or lose the power. Flash simulator only.
 *
 *          Each test builds a device, remembers the partition, and then runs
 *          one operation over and over from that same starting point, with
 *          the fault moved a little further along each time, until the
 *          operation gets through untouched. After every run the device is
 *          attached again and has to make sense.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "partition.h"
#include "suite.h"
#include "sweep.h"

/* Module defines ---------------------------------------------------------- */

/** Rounds of churn after which the hot pair stands past the threshold. */
#define WEAR_OUT_ROUNDS (2 * (CONFIG_UBI_WEAR_LEVELING_THRESHOLD + 1))

/** Seed the cold volume is written from. */
#define COLD_SEED (0x30)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Fill every logical block of a new volume and wear a pair of blocks
 *        out, so that relocation has work to do.
 *
 * \param[out] leb_count                Logical blocks that hold data.
 *
 * \return Identifier the volume was given.
 */
static uint32_t cold_volume_ready(uint32_t *leb_count);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_power_loss_leb);

/* Static function definitions --------------------------------------------- */

static uint32_t cold_volume_ready(uint32_t *leb_count)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_volume_config wanted = { .name = "cold", .leb_count = 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_ok(ubi_device_format(&config));
	sweep_attach();
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	volume_fill(vol_id, 0, wanted.leb_count, COLD_SEED);

	/* Every block is mapped, so the churn stays on one pair. */
	leb_payload(COLD_SEED, 0, written, sizeof(written));

	for (uint32_t round = 0; round < WEAR_OUT_ROUNDS; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, written,
					  sizeof(written)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
	}

	zassert_ok(ubi_leb_unmap(ubi, vol_id, wanted.leb_count - 1));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(0 < info.relocatable_pebs,
		     "relocation needs something to do");

	*leb_count = wanted.leb_count - 1;

	return vol_id;
}

/* Module interface function definitions ----------------------------------- */

/* Tests: a change --------------------------------------------------------- */

/*
 * Given: a logical block holding one set of contents.
 * When:  a change to another set is cut short by a power loss, at every
 *        point along the way.
 * Then:  after the reboot the block holds either the old contents or the new
 *        ones, never a mixture and never nothing.
 */
ZTEST(ubi_power_loss_leb, test_a_change_cut_short_leaves_the_old_or_the_new)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x13);
	pattern_fill(fresh, sizeof(fresh), 0x24);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t cut = 0;; cut += SWEEP_STRIDE) {
		flash_snapshot_restore();
		sweep_attach();

		flash_power_cut_after(cut);
		const int ret =
			ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh));
		const bool cut_short = flash_power_is_cut();

		sweep_reboot();
		sweep_attach();

		if (!cut_short) {
			zassert_ok(ret);
			leb_check(vol_id, 0, fresh, sizeof(fresh));
			zassert_ok(ubi_device_deinit(ubi));
			break;
		}

		zassert_true(leb_holds(vol_id, 0, old, sizeof(old)) ||
				     leb_holds(vol_id, 0, fresh, sizeof(fresh)),
			     "a cut after %u bytes left neither version", cut);

		zassert_ok(ubi_device_deinit(ubi));
	}
}

/*
 * Given: a logical block holding one set of contents.
 * When:  a change to another set fails on one write, at every point along
 *        the way, and the device carries on.
 * Then:  what the device serves right after the failure is what the next
 *        attach finds, and it goes on writing without ever writing twice
 *        over the same bytes.
 */
ZTEST(ubi_power_loss_leb, test_a_failed_change_leaves_ram_and_flash_agreeing)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x35);
	pattern_fill(fresh, sizeof(fresh), 0x46);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t budget = 0;; budget += SWEEP_STRIDE) {
		flash_snapshot_restore();
		sweep_attach();

		flash_fail_one_write_after(budget);
		const int ret =
			ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh));
		const bool failed = flash_fault_fired();

		flash_faults_clear();

		const bool served_fresh =
			leb_holds(vol_id, 0, fresh, sizeof(fresh));

		zassert_equal(0 == ret, served_fresh,
			      "a failure after %u bytes returned %d", budget,
			      ret);
		zassert_true(served_fresh ||
			     leb_holds(vol_id, 0, old, sizeof(old)));

		/* The block the failed write took must not be written again
		 * before an erase. */
		zassert_ok(ubi_leb_change(ubi, vol_id, 1, old, sizeof(old)));

		sweep_reboot();
		sweep_attach();

		zassert_equal(served_fresh,
			      leb_holds(vol_id, 0, fresh, sizeof(fresh)),
			      "a failure after %u bytes split RAM from flash",
			      budget);
		leb_check(vol_id, 1, old, sizeof(old));

		zassert_ok(ubi_device_deinit(ubi));

		if (!failed)
			break;
	}
}

/*
 * Given: a logical block that was never written.
 * When:  its first change fails on one write, at every point along the way,
 *        and the device is attached again.
 * Then:  the attach finds what the device served right after the failure: a
 *        change that failed leaves no part of itself behind for the attach to
 *        keep.
 */
ZTEST(ubi_power_loss_leb, test_a_failed_first_change_leaves_nothing_behind)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(fresh, sizeof(fresh), 0x57);

	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t budget = 0;; budget += SWEEP_STRIDE) {
		struct ubi_leb_info info = { 0 };

		flash_snapshot_restore();
		sweep_attach();

		flash_fail_one_write_after(budget);
		const int ret =
			ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh));
		const bool failed = flash_fault_fired();

		flash_faults_clear();

		zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
		zassert_equal(0 == ret, info.mapped,
			      "a failure after %u bytes returned %d", budget,
			      ret);

		sweep_reboot();
		sweep_attach();

		zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
		zassert_equal(
			0 == ret, info.mapped,
			"a failure after %u bytes left part of the change "
			"behind",
			budget);

		if (0 == ret)
			leb_check(vol_id, 0, fresh, sizeof(fresh));

		zassert_ok(ubi_device_deinit(ubi));

		if (!failed)
			break;
	}
}

/* Tests: an erase --------------------------------------------------------- */

/*
 * Given: a logical block changed twice, so the copy the second change
 *        replaced still waits for reclaim.
 * When:  erasing the block is cut short by a power loss, at every point
 *        along the way.
 * Then:  after the reboot the block holds its last contents or nothing,
 *        never the ones the change replaced: the older copy goes first.
 */
ZTEST(ubi_power_loss_leb,
      test_an_erase_cut_short_never_brings_back_older_contents)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0x71);
	pattern_fill(fresh, sizeof(fresh), 0x82);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			struct ubi_leb_info info = { 0 };

			flash_snapshot_restore();
			sweep_attach();

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret = ubi_leb_erase(ubi, vol_id, 0);
			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the erase failed with %d", ret);

			sweep_reboot();
			sweep_attach();

			zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));

			if (info.mapped)
				zassert_true(
					leb_holds(vol_id, 0, fresh,
						  sizeof(fresh)),
					"a cut %s %u brought back what the "
					"change replaced",
					where, cut);

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;
		}
	}
}

/*
 * Given: a logical block changed twice, the newer copy below the older one
 *        in the partition, and then unmapped, so both copies wait for
 *        reclaim.
 * When:  erasing the block is cut short by a power loss, at every point
 *        along the way.
 * Then:  after the reboot the block holds its last contents or nothing:
 *        whatever order the copies sit in, the older one goes first.
 */
ZTEST(ubi_power_loss_leb, test_an_erase_cut_short_after_an_unmap_keeps_the_last)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t spacer[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(spacer, sizeof(spacer), 0x93);
	pattern_fill(old, sizeof(old), 0xA4);
	pattern_fill(fresh, sizeof(fresh), 0xB5);

	/* The spacer holds the free block the table left while the old copy
	 * goes above it, then hands it on to the new one. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, spacer, sizeof(spacer)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_erase(ubi, vol_id, 1));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_true(pnum_of_data_matching(fresh, sizeof(fresh)) <
			     pnum_of_data_matching(old, sizeof(old)),
		     "the newer copy has to sit below the older one");

	flash_snapshot_take();

	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			struct ubi_leb_info info = { 0 };

			flash_snapshot_restore();
			sweep_attach();
			zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret = ubi_leb_erase(ubi, vol_id, 0);
			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the erase failed with %d", ret);

			sweep_reboot();
			sweep_attach();

			zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));

			if (info.mapped)
				zassert_true(
					leb_holds(vol_id, 0, fresh,
						  sizeof(fresh)),
					"a cut %s %u brought back what the "
					"change replaced",
					where, cut);

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;
		}
	}
}

/*
 * Given: a logical block whose newer copy fails its seal, so the attach took
 *        the older one and left the newer waiting for reclaim, and which was
 *        then unmapped.
 * When:  erasing the block is cut short by a power loss, at every point
 *        along the way.
 * Then:  after the reboot the block holds the contents it had or nothing:
 *        the copy the attach turned down goes before the one it took.
 */
ZTEST(ubi_power_loss_leb, test_an_erase_cut_short_never_brings_back_a_torn_copy)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t old[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(old, sizeof(old), 0xC6);
	pattern_fill(fresh, sizeof(fresh), 0xD7);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	flash_snapshot_take();

	for (uint32_t in_erase = 0; in_erase < 2; ++in_erase) {
		const char *where = in_erase ? "in erase" : "after byte";

		for (uint32_t cut = 0;; cut += in_erase ? 1 : SWEEP_STRIDE) {
			struct ubi_leb_info info = { 0 };

			flash_snapshot_restore();
			sweep_attach();
			leb_check(vol_id, 0, old, sizeof(old));
			zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

			if (in_erase)
				flash_power_cut_during_erase(cut);
			else
				flash_power_cut_after(cut);

			const int ret = ubi_leb_erase(ubi, vol_id, 0);
			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the erase failed with %d", ret);

			sweep_reboot();
			sweep_attach();

			zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));

			if (info.mapped)
				zassert_true(
					leb_holds(vol_id, 0, old, sizeof(old)),
					"a cut %s %u brought back the copy "
					"the attach turned down",
					where, cut);

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;
		}
	}
}

/* Without the headers invalidated first, a torn erase can leave them over
 * whatever part of the block it did not reach. */
#if defined(CONFIG_UBI_ERASE_INVALIDATES_HEADERS)

/*
 * Given: a logical block that was appended to, at its start and at its end,
 *        so no seal vouches for it.
 * When:  its block is erased, by an erase or by a reclaim after an unmap,
 *        and the power goes halfway through the erase.
 * Then:  after the reboot the block is either gone or whole: a half-erased
 *        block must never come back as the logical block's contents.
 */
ZTEST(ubi_power_loss_leb,
      test_an_erase_cut_short_never_brings_back_half_a_block)
{
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info device = { 0 };
	struct ubi_leb_info info = { 0 };
	uint8_t head[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t tail[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	pattern_fill(head, sizeof(head), 0x79);
	pattern_fill(tail, sizeof(tail), 0x8A);

	zassert_ok(ubi_device_get_info(ubi, &device));

	const uint32_t tail_at = device.leb_size - sizeof(tail);

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, head, sizeof(head)));
	zassert_ok(
		ubi_leb_write_at(ubi, vol_id, 0, tail_at, tail, sizeof(tail)));
	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t by_reclaim = 0; by_reclaim < 2; ++by_reclaim) {
		for (uint32_t erases = 0;; ++erases) {
			int ret = 0;

			flash_snapshot_restore();
			sweep_attach();

			flash_power_cut_during_erase(erases);

			if (by_reclaim) {
				zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
				ret = ubi_maintenance(ubi,
						      UBI_MAINTENANCE_RECLAIM,
						      1, &result);
			} else {
				ret = ubi_leb_erase(ubi, vol_id, 0);
			}

			const bool cut_short = flash_power_is_cut();

			zassert_true(0 == ret || cut_short,
				     "the erase failed with %d", ret);

			sweep_reboot();
			sweep_attach();

			zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));

			if (info.mapped) {
				zassert_ok(ubi_leb_read(ubi, vol_id, 0, tail_at,
							read, sizeof(read)));
				zassert_mem_equal(tail, read, sizeof(tail),
						  "a block cut short in erase "
						  "%u came back half erased",
						  erases);
			}

			zassert_ok(ubi_device_deinit(ubi));

			if (!cut_short)
				break;
		}
	}
}

#endif /* CONFIG_UBI_ERASE_INVALIDATES_HEADERS */

/* Tests: a relocation ----------------------------------------------------- */

/*
 * Given: an unevenly worn device whose cold data relocation is about to move.
 * When:  the relocation is cut short by a power loss, at every point along
 *        the way.
 * Then:  every logical block still reads what it held after the reboot.
 */
ZTEST(ubi_power_loss_leb, test_a_relocation_cut_short_loses_nothing)
{
	struct ubi_maintenance_result result = { 0 };
	uint32_t leb_count = 0;
	const uint32_t vol_id = cold_volume_ready(&leb_count);

	zassert_ok(ubi_device_deinit(ubi));

	flash_snapshot_take();

	for (uint32_t cut = 0;; cut += SWEEP_STRIDE) {
		flash_snapshot_restore();
		sweep_attach();

		flash_power_cut_after(cut);
		const int ret = ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE,
						1, &result);
		const bool cut_short = flash_power_is_cut();

		zassert_true(0 == ret || cut_short,
			     "the relocation failed with %d", ret);

		sweep_reboot();
		sweep_attach();

		volume_check(vol_id, 0, leb_count, COLD_SEED);

		zassert_ok(ubi_device_deinit(ubi));

		if (!cut_short)
			break;
	}
}
