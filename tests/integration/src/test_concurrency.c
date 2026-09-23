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
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
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

/* Static function declarations -------------------------------------------- */

/**
 * \brief Rewrite one logical block over and over, keeping maintenance up,
 *        and report the first refusal rather than asserting.
 */
static int drive(uint32_t lnum, uint8_t seed);

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

/* Static function definitions --------------------------------------------- */

static int drive(uint32_t lnum, uint8_t seed)
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
		if (0 != memcmp(written, read, sizeof(read)))
			return -EBADMSG;

		ret = ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result);

		if (0 != ret)
			return ret;
	}

	return 0;
}

static void worker(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	worker_result = drive(WORKER_LEB, WORKER_SEED);
}

/* Module interface function definitions ----------------------------------- */

/*
 * Given: two threads sharing one attached handle, each rewriting a logical
 *        block of its own and keeping maintenance up.
 * When:  they run at the same time.
 * Then:  neither is refused, neither sees the other's bytes, and each block
 *        holds what its own thread wrote last, before and after a reattach.
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

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &worker_vol_id));

	k_thread_create(&worker_thread, worker_stack, WORKER_STACK_SIZE, worker,
			NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);

	const int main_result = drive(MAIN_LEB, MAIN_SEED);

	zassert_ok(k_thread_join(&worker_thread, K_FOREVER));

	zassert_ok(main_result, "the test thread was refused");
	zassert_ok(worker_result, "the worker was refused");

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
