/**
 * \file    test_concurrency.c
 * \author  Kamil Kielbasa
 * \brief   Two threads through one handle.
 *
 *          A failed assertion aborts the test thread, so the worker only
 *          reports and the test thread asserts once it has joined.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Room for one UBI operation and the flash driver beneath it. */
#define WORKER_STACK_SIZE (8192)

/** Rewrites each thread makes. */
#define WORKER_ROUNDS (256)

/** Logical block the test thread rewrites, and the pattern it writes. */
#define MAIN_LEB (0)
#define MAIN_SEED (0xA1)

/** Logical block the worker rewrites, and the pattern it writes. */
#define WORKER_LEB (1)
#define WORKER_SEED (0xC2)

#if defined(CONFIG_FLASH_SIMULATOR)

/** Bytes written between two switches of thread, on the simulator. */
#define YIELD_EVERY (64)

#endif /* CONFIG_FLASH_SIMULATOR */

/* Static function declarations -------------------------------------------- */

/**
 * \brief Rewrite one logical block over and over, keeping maintenance up,
 *        and report the first refusal rather than asserting.
 *
 * \param[out] rounds                   Counts the rounds finished.
 */
static int drive(uint32_t lnum, uint8_t seed, atomic_t *rounds);

/**
 * \brief Thread entry: drive() with the worker's block and pattern.
 */
static void worker(void *unused1, void *unused2, void *unused3);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_concurrency);

static K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);
static struct k_thread worker_thread = { 0 };

/** Volume both threads work in. */
static uint32_t worker_vol_id = UBI_VOL_ID_INVALID;

/** What drive() returned on the worker. */
static int worker_result = 0;

/** Rounds each thread has finished. */
static atomic_t main_rounds = ATOMIC_INIT(0);
static atomic_t worker_rounds = ATOMIC_INIT(0);

/** Rounds the test thread had finished when the worker began. */
static atomic_val_t main_rounds_at_worker_start = 0;

/* Static function definitions --------------------------------------------- */

static int drive(uint32_t lnum, uint8_t seed, atomic_t *rounds)
{
	struct ubi_maintenance_result result = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	int ret = 0;

	pattern_fill(written, sizeof(written), seed);

	for (uint32_t round = 0; round < WORKER_ROUNDS; ++round) {
		ret = ubi_leb_change(ubi, worker_vol_id, lnum, written,
				     sizeof(written));

		if (0 != ret)
			return ret;

		ret = ubi_leb_read(ubi, worker_vol_id, lnum, 0, read,
				   sizeof(read));

		if (0 != ret)
			return ret;

		/* The other thread's block must never show through this one. */
		const bool same = (0 == memcmp(written, read, sizeof(read)));

		if (!same)
			return -EBADMSG;

		ret = ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result);

		if (0 != ret)
			return ret;

		atomic_inc(rounds);
	}

	return 0;
}

static void worker(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	main_rounds_at_worker_start = atomic_get(&main_rounds);
	worker_result = drive(WORKER_LEB, WORKER_SEED, &worker_rounds);
}

/* Module interface function definitions ----------------------------------- */

/*
 * Given: two threads of the same priority sharing one attached handle, each
 *        rewriting a logical block of its own and keeping maintenance up.
 * When:  they run at the same time, switching in the middle of flash writes
 *        on the simulator and wherever the driver waits on hardware.
 * Then:  their work overlaps, neither is refused, neither sees the other's
 *        bytes, and each block holds what its own thread wrote last, before
 *        and after a reattach.
 */
ZTEST(ubi_concurrency, test_two_threads_may_share_one_handle)
{
	const struct ubi_volume_config wanted = {
		.name = "shared", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint8_t main_expected[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t worker_expected[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(main_expected, sizeof(main_expected), MAIN_SEED);
	pattern_fill(worker_expected, sizeof(worker_expected), WORKER_SEED);

	worker_vol_id = UBI_VOL_ID_INVALID;
	worker_result = 0;
	atomic_clear(&main_rounds);
	atomic_clear(&worker_rounds);
	main_rounds_at_worker_start = WORKER_ROUNDS;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &worker_vol_id));

#if defined(CONFIG_FLASH_SIMULATOR)
	flash_yield_every(YIELD_EVERY);
#endif

	k_thread_create(&worker_thread, worker_stack, WORKER_STACK_SIZE, worker,
			NULL, NULL, NULL,
			k_thread_priority_get(k_current_get()), 0, K_NO_WAIT);

	const int main_result = drive(MAIN_LEB, MAIN_SEED, &main_rounds);
	const atomic_val_t worker_rounds_at_main_end =
		atomic_get(&worker_rounds);

	zassert_ok(k_thread_join(&worker_thread, K_FOREVER));

#if defined(CONFIG_FLASH_SIMULATOR)
	flash_yield_every(0);
#endif

	zassert_ok(main_result, "the test thread was refused");
	zassert_ok(worker_result, "the worker was refused");

#if defined(CONFIG_FLASH_SIMULATOR)
	zassert_true(main_rounds_at_worker_start < WORKER_ROUNDS,
		     "the worker has to start while the test thread is busy");
	zassert_true(0 < worker_rounds_at_main_end,
		     "and get work done before the test thread is through");
#else
	ARG_UNUSED(worker_rounds_at_main_end);
#endif

	zassert_ok(ubi_leb_read(ubi, worker_vol_id, MAIN_LEB, 0, read,
				sizeof(read)));
	zassert_mem_equal(main_expected, read, sizeof(read));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, worker_vol_id, WORKER_LEB, 0, read,
				sizeof(read)));
	zassert_mem_equal(worker_expected, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, worker_vol_id, MAIN_LEB, 0, read,
				sizeof(read)));
	zassert_mem_equal(main_expected, read, sizeof(read),
			  "what was written has to be on the flash");

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, worker_vol_id, WORKER_LEB, 0, read,
				sizeof(read)));
	zassert_mem_equal(worker_expected, read, sizeof(read),
			  "what was written has to be on the flash");

	zassert_ok(ubi_device_deinit(ubi));
}
