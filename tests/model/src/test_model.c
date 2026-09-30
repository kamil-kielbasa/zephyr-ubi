/**
 * \file    test_model.c
 * \author  Kamil Kielbasa
 * \brief   Every operation, in a random order, against a model of what the
 *          device has to hold.
 *
 *          A seeded generator picks each operation and its arguments, and
 *          the model says what the device has to answer and hold after it.
 *          On the flash simulator some operations are cut short by a power
 *          loss or by a failed write; the device then has to hold what it
 *          held before or what the operation would have left, and a write
 *          that is not atomic may stop anywhere.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "model.h"
#include "model_step.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_model);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a formatted device and a model of what it holds.
 * When:  operations of every kind are carried out in a random order, some
 *        of them cut short by a power loss or by a failed write.
 * Then:  the device always answers and holds what the model says, and it
 *        never writes twice over the same bytes.
 */
ZTEST(ubi_model, test_the_device_holds_what_the_model_says)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	attach_timed();
	zassert_ok(ubi_device_get_info(ubi, &info));

	leb_size = info.leb_size;
	write_block = info.write_block_size;
	lebs_shared = info.free_lebs;

	zassert_true(MODEL_SEGMENTS * MODEL_WRITE_MAX <= leb_size);
	zassert_equal(0, MODEL_WRITE_MAX % write_block);

	printk("model: seed 0x%08x, %u steps, %u blocks of %u bytes written "
	       "%u at a time\n",
	       (uint32_t)CONFIG_UBI_TEST_MODEL_SEED,
	       (uint32_t)CONFIG_UBI_TEST_MODEL_STEPS, info.peb_count,
	       info.peb_size, write_block);

	for (step = 0; step < CONFIG_UBI_TEST_MODEL_STEPS; ++step)
		step_run();

	reattach();
	model_verify();

	zassert_equal(0, event_count[UBI_EVENT_HDR_TAMPERED],
		      "nothing was tampered with");
	zassert_equal(0, event_count[UBI_EVENT_LEB_ORPHANED],
		      "no block claims a volume the device never created");

	for (uint32_t op = 0; op < OP_COUNT; ++op)
		printk("model: %u %s\n", stats.done[op], op_name[op]);

	printk("model: %u refused as expected, %u for want of space, %u cut "
	       "short, %u failed writes, %u left part way, %u relocated\n",
	       stats.refused, stats.no_space, stats.cuts, stats.failures,
	       stats.partial, stats.relocated);
	printk("model: attach of %u blocks took %u ms, %u ms at most\n",
	       info.peb_count, stats.attach_last, stats.attach_max);

	zassert_ok(ubi_device_deinit(ubi));
}
