/**
 * \file    test_stack.c
 * \author  Kamil Kielbasa
 * \brief   Stack the deepest calls use, on a thread of their own.
 *
 *          Every public call runs at least once, each maintenance operation
 *          with work to do, and an attach meets a damaged volume table and a
 *          corrupt block. The part of the thread's stack left untouched says
 *          how much was used, which must stay within
 *          \c CONFIG_UBI_TEST_STACK_BUDGET.
 *
 *          native_sim runs threads on the host's stack, so this runs on a
 *          Cortex-M33 instead: mps2/an521/cpu0 under QEMU in CI, and the
 *          nRF5340 DK.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "partition.h"
#include "suite.h"
#include "table_copies.h"

/* Module defines ---------------------------------------------------------- */

/** Room past the budget, so that going over is measured, not a crash. */
#define WORKER_STACK_SIZE (CONFIG_UBI_TEST_STACK_BUDGET + 1024)

/** Rounds of change and reclaim that make a relocation due. */
#define WEAR_OUT_ROUNDS (2 * (CONFIG_UBI_WEAR_LEVELING_THRESHOLD + 1))

/* Static function declarations -------------------------------------------- */

/**
 * \brief Run \p entry on the worker thread and wait for it.
 *
 * \return Bytes of the worker's stack it used.
 */
static size_t worker_run(k_thread_entry_t entry);

/**
 * \brief Every call on a device formatted for the purpose, ending detached
 *        with one more block to find corrupt.
 */
static void calls_on_a_fresh_device(void *p1, void *p2, void *p3);

/**
 * \brief An attach that meets damage, and the maintenance that clears it.
 */
static void calls_on_a_damaged_device(void *p1, void *p2, void *p3);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_stack);

K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);

static struct k_thread worker;

/* Payloads are kept off the stack being measured. */
static uint8_t cold[UBI_TEST_PAYLOAD_SIZE];
static uint8_t hot[UBI_TEST_PAYLOAD_SIZE];
static uint8_t marked[UBI_TEST_PAYLOAD_SIZE];
static uint8_t read_back[UBI_TEST_PAYLOAD_SIZE];

/* Static function definitions --------------------------------------------- */

static size_t worker_run(k_thread_entry_t entry)
{
	size_t unused = 0;

	k_thread_create(&worker, worker_stack,
			K_THREAD_STACK_SIZEOF(worker_stack), entry, NULL, NULL,
			NULL, k_thread_priority_get(k_current_get()), 0,
			K_NO_WAIT);

	zassert_ok(k_thread_join(&worker, K_FOREVER));
	zassert_ok(k_thread_stack_space_get(&worker, &unused));

	return worker.stack_info.size - unused;
}

static void calls_on_a_fresh_device(void *p1, void *p2, void *p3)
{
	const struct ubi_volume_config scratch = { .name = "scratch",
						   .leb_count = 1 };
	struct ubi_volume_config wanted = { .name = "cold", .leb_count = 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	struct ubi_volume_info volume = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint32_t scratch_id = UBI_VOL_ID_INVALID;
	uint32_t last = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	wanted.leb_count = info.free_lebs;
	last = wanted.leb_count - 1;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 1; lnum < last; ++lnum)
		zassert_ok(
			ubi_leb_change(ubi, vol_id, lnum, cold, sizeof(cold)));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, last, 0, cold, sizeof(cold)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, last, sizeof(cold), cold,
				    sizeof(cold)));

	/* The few spare blocks take all the wear. */
	for (uint32_t round = 0; round < WEAR_OUT_ROUNDS; ++round) {
		zassert_ok(ubi_leb_change(ubi, vol_id, 0, hot, sizeof(hot)));
		zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1,
					   &result));
	}

	zassert_ok(ubi_leb_unmap(ubi, vol_id, last));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(0 < info.relocatable_pebs, "no relocation is due");
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &result));
	zassert_equal(1, result.performed);

	zassert_ok(
		ubi_leb_read(ubi, vol_id, 1, 0, read_back, sizeof(read_back)));
	zassert_mem_equal(cold, read_back, sizeof(cold));
	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &leb));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &volume));
	zassert_ok(ubi_volume_find(ubi, "cold", &vol_id));

	zassert_ok(ubi_leb_erase(ubi, vol_id, 2));
	zassert_ok(ubi_leb_map(ubi, vol_id, 2));
	zassert_ok(ubi_volume_resize(ubi, vol_id, last));
	zassert_ok(ubi_volume_create(ubi, &scratch, &scratch_id));
	zassert_ok(ubi_volume_remove(ubi, scratch_id));

	zassert_ok(ubi_leb_change(ubi, vol_id, 3, marked, sizeof(marked)));
	zassert_ok(ubi_device_deinit(ubi));
}

static void calls_on_a_damaged_device(void *p1, void *p2, void *p3)
{
	struct ubi_maintenance_result result = { 0 };

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(1, result.performed);
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_DISCARD, 1, &result));
	zassert_equal(1, result.performed);
	zassert_ok(ubi_device_deinit(ubi));
}

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a thread whose stack starts out filled with a pattern.
 * When:  it formats, attaches, calls everything, attaches to damage and
 *        maintains the device.
 * Then:  it never used more stack than the documented budget.
 */
ZTEST(ubi_stack, test_the_deepest_calls_fit_the_budget)
{
	size_t used = 0;

	pattern_fill(cold, sizeof(cold), 0xC0);
	pattern_fill(hot, sizeof(hot), 0x40);
	pattern_fill(marked, sizeof(marked), 0x3A);

	used = MAX(used, worker_run(calls_on_a_fresh_device));

	zassert_equal(1, corrupt_volume_tables(config.ikm_key_id, 1));
	zassert_equal(1,
		      corrupt_header_of_data_matching(marked, sizeof(marked)));

	used = MAX(used, worker_run(calls_on_a_damaged_device));

	TC_PRINT("stack used: %zu of %d bytes budgeted\n", used,
		 CONFIG_UBI_TEST_STACK_BUDGET);
	zassert_true(used <= CONFIG_UBI_TEST_STACK_BUDGET,
		     "%zu bytes of stack used", used);
}
