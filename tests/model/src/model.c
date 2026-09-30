/**
 * \file    model.c
 * \author  Kamil Kielbasa
 * \brief   What the device has to hold, and checking that it does.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "model.h"
#include "partition.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Erased bytes checked past what a block holds. */
#define MODEL_TAIL (64)

/* Static function declarations -------------------------------------------- */

/**
 * \brief What byte \p at of a block has to read.
 */
static uint8_t leb_byte(const struct model_leb *leb, uint32_t at);

/**
 * \brief Report whether bytes \p from to \p to of a block read as \p leb
 *        says.
 */
static bool leb_reads_range(const struct model_volume *volume, uint32_t lnum,
			    const struct model_leb *leb, uint32_t from,
			    uint32_t to);

/**
 * \brief Settle a block after an attach: a copy an unmap let go of has come
 *        back or it has not.
 */
static void leb_settle(const struct model_volume *volume, uint32_t lnum,
		       struct model_leb *leb);

/**
 * \brief Settle the block an operation cut short was writing.
 */
static void leb_settle_target(const struct model_volume *volume, uint32_t lnum,
			      const struct model_leb *old,
			      const struct model_leb *new, bool partial,
			      struct model_leb *leb);

/* Module variables and constants ------------------------------------------ */

struct model model = { 0 };

struct model before = { 0 };

uint32_t step = 0;

uint32_t leb_size = 0;

uint32_t write_block = 0;

uint32_t lebs_shared = 0;

struct model_stats stats = { 0 };

const char *const op_name[OP_COUNT] = {
	[OP_CHANGE] = "change",	    [OP_APPEND] = "append",
	[OP_READ] = "read",	    [OP_MAP] = "map",
	[OP_UNMAP] = "unmap",	    [OP_ERASE] = "erase",
	[OP_CREATE] = "create",	    [OP_REMOVE] = "remove",
	[OP_RESIZE] = "resize",	    [OP_MAINTAIN] = "maintain",
	[OP_REATTACH] = "reattach",
};

/** State of the generator. */
static uint32_t random_state = CONFIG_UBI_TEST_MODEL_SEED;

/** Bytes read back. */
static uint8_t readback[MODEL_WRITE_MAX + MODEL_TAIL] = { 0 };

/* Static function definitions --------------------------------------------- */

static uint8_t leb_byte(const struct model_leb *leb, uint32_t at)
{
	uint32_t start = 0;

	if (!leb->mapped)
		return UBI_TEST_ERASED;

	for (uint32_t i = 0; i < leb->segments; ++i) {
		const struct model_segment *segment = &leb->segment[i];

		if (at < start + segment->length)
			return segment_byte(segment->seed, at - start);

		start += segment->length;
	}

	return UBI_TEST_ERASED;
}

static bool leb_reads_range(const struct model_volume *volume, uint32_t lnum,
			    const struct model_leb *leb, uint32_t from,
			    uint32_t to)
{
	for (uint32_t at = from; at < to; at += sizeof(readback)) {
		const uint32_t length = MIN(sizeof(readback), to - at);

		zassert_ok(ubi_leb_read(ubi, volume->vol_id, lnum, at, readback,
					length),
			   "step %u: volume %u block %u will not read", step,
			   volume->vol_id, lnum);

		for (uint32_t i = 0; i < length; ++i) {
			const uint8_t expected = leb_byte(leb, at + i);

			if (expected != readback[i])
				return false;
		}
	}

	return true;
}

static void leb_settle(const struct model_volume *volume, uint32_t lnum,
		       struct model_leb *leb)
{
	struct ubi_leb_info info = { 0 };

	if (!leb->ghost) {
		zassert_true(leb_reads(volume, lnum, leb),
			     "step %u: volume %u block %u does not hold what "
			     "the model has",
			     step, volume->vol_id, lnum);
		return;
	}

	zassert_ok(ubi_leb_get_info(ubi, volume->vol_id, lnum, &info));

	leb->ghost = false;

	if (!info.mapped) {
		*leb = (struct model_leb){ 0 };
		return;
	}

	leb->mapped = true;

	zassert_true(leb_reads(volume, lnum, leb),
		     "step %u: volume %u block %u came back with contents it "
		     "did not have when it was unmapped",
		     step, volume->vol_id, lnum);
}

static void leb_settle_target(const struct model_volume *volume, uint32_t lnum,
			      const struct model_leb *old,
			      const struct model_leb *new, bool partial,
			      struct model_leb *leb)
{
	struct ubi_leb_info info = { 0 };
	struct model_leb ghost = *old;

	zassert_ok(ubi_leb_get_info(ubi, volume->vol_id, lnum, &info));

	if (!info.mapped) {
		zassert_true(!old->mapped || !new->mapped,
			     "step %u: volume %u block %u lost its contents",
			     step, volume->vol_id, lnum);
		*leb = (struct model_leb){ 0 };
		return;
	}

	const bool holds_new = new->mapped &&leb_reads(volume, lnum, new);

	if (holds_new) {
		*leb = *new;
		return;
	}

	const bool holds_old = old->mapped && leb_reads(volume, lnum, old);

	if (holds_old) {
		*leb = *old;
		return;
	}

	ghost.mapped = true;
	ghost.ghost = false;

	const bool holds_ghost = old->ghost && leb_reads(volume, lnum, &ghost);

	if (holds_ghost) {
		*leb = ghost;
		return;
	}

	const bool holds_part =
		partial && new->mapped &&
		0 != new->segments &&leb_reads_cut(volume, lnum, new, leb);

	if (holds_part) {
		stats.partial += 1;
		return;
	}

	zassert_unreachable("step %u: volume %u block %u holds neither what "
			    "it held nor what was written to it",
			    step, volume->vol_id, lnum);
}

/* Module interface function definitions ----------------------------------- */

uint32_t random_next(void)
{
	uint32_t x = random_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	random_state = x;

	return x;
}

uint32_t random_below(uint32_t bound)
{
	return random_next() % bound;
}

uint8_t segment_byte(uint32_t seed, uint32_t at)
{
	uint32_t x = seed + at * 0x9E3779B1UL;

	x ^= x >> 15;
	x *= 0x2C1B3C6DUL;
	x ^= x >> 12;

	return (uint8_t)x;
}

uint32_t leb_written(const struct model_leb *leb)
{
	uint32_t written = 0;

	for (uint32_t i = 0; i < leb->segments; ++i)
		written += leb->segment[i].length;

	return written;
}

void volume_name(uint32_t slot, char name[MODEL_NAME_SIZE])
{
	const int length = snprintf(name, MODEL_NAME_SIZE, "v%u", slot);

	zassert_between_inclusive(length, 2, MODEL_NAME_SIZE - 1);
}

bool leb_reads(const struct model_volume *volume, uint32_t lnum,
	       const struct model_leb *leb)
{
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_get_info(ubi, volume->vol_id, lnum, &info),
		   "step %u: volume %u block %u is not there", step,
		   volume->vol_id, lnum);

	if (info.mapped != leb->mapped)
		return false;

	return leb_reads_range(volume, lnum, leb, 0,
			       MIN(leb_size, leb_written(leb) + MODEL_TAIL));
}

bool leb_reads_cut(const struct model_volume *volume, uint32_t lnum,
		   const struct model_leb *leb, struct model_leb *found)
{
	const struct model_segment *last = &leb->segment[leb->segments - 1];
	const uint32_t start = leb_written(leb) - last->length;
	const uint32_t reach = MIN(last->length + MODEL_TAIL, leb_size - start);
	const bool head_holds = leb_reads_range(volume, lnum, leb, 0, start);
	uint32_t kept = 0;

	if (!head_holds)
		return false;

	zassert_ok(ubi_leb_read(ubi, volume->vol_id, lnum, start, readback,
				reach),
		   "step %u: volume %u block %u will not read", step,
		   volume->vol_id, lnum);

	/* A write cut short stops at a byte; everything after it is still
	 * erased. */
	for (uint32_t i = 0; i < reach; ++i) {
		if (UBI_TEST_ERASED != readback[i])
			kept = i + 1;
	}

	if (kept > last->length)
		return false;

	for (uint32_t i = 0; i < kept; ++i) {
		const uint8_t expected = segment_byte(last->seed, i);

		if (expected != readback[i])
			return false;
	}

	*found = *leb;
	found->segment[leb->segments - 1].length = kept;
	found->closed = true;

	if (0 == kept)
		found->segments -= 1;

	return true;
}

bool volumes_are(struct model *expected)
{
	struct ubi_device_info info = { 0 };
	uint32_t count = 0;
	uint32_t claimed = 0;

	for (uint32_t slot = 0; slot < MODEL_VOLUMES; ++slot) {
		struct model_volume *volume = &expected->volume[slot];
		struct ubi_volume_info found = { 0 };
		char name[MODEL_NAME_SIZE] = { 0 };
		uint32_t vol_id = UBI_VOL_ID_INVALID;

		volume_name(slot, name);

		const int ret = ubi_volume_find(ubi, name, &vol_id);

		if (!volume->exists) {
			if (-ENOENT != ret)
				return false;

			continue;
		}

		if (0 != ret)
			return false;

		zassert_ok(ubi_volume_get_info(ubi, vol_id, &found));

		if (found.leb_count != volume->leb_count)
			return false;

		if (UBI_VOL_ID_INVALID != volume->vol_id &&
		    vol_id != volume->vol_id)
			return false;

		volume->vol_id = vol_id;
		count += 1;
		claimed += volume->leb_count;
	}

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(count, info.volume_count, "step %u", step);
	zassert_equal(lebs_shared - claimed, info.free_lebs,
		      "step %u: the device shares out logical blocks the "
		      "volumes do not have",
		      step);

	return true;
}

void model_verify(void)
{
	zassert_true(volumes_are(&model),
		     "step %u: the volumes are not the ones the model has",
		     step);

	for (uint32_t slot = 0; slot < MODEL_VOLUMES; ++slot) {
		const struct model_volume *volume = &model.volume[slot];

		if (!volume->exists)
			continue;

		for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum)
			zassert_true(leb_reads(volume, lnum,
					       &volume->leb[lnum]),
				     "step %u: volume %u block %u does not "
				     "hold what the model has",
				     step, volume->vol_id, lnum);
	}
}

void attach_timed(void)
{
	const int64_t start = k_uptime_get();

	zassert_ok(ubi_device_init(ubi, &config),
		   "step %u: the device would not attach", step);

	stats.attach_last = (uint32_t)(k_uptime_get() - start);
	stats.attach_max = MAX(stats.attach_max, stats.attach_last);
}

void model_settle(struct model *settled, const struct model_step *s,
		  const struct model *old)
{
	const bool one_block = OP_CHANGE == s->op || OP_APPEND == s->op ||
			       OP_MAP == s->op || OP_ERASE == s->op;

	for (uint32_t slot = 0; slot < MODEL_VOLUMES; ++slot) {
		struct model_volume *volume = &settled->volume[slot];

		if (!volume->exists)
			continue;

		for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum) {
			struct model_leb *leb = &volume->leb[lnum];

			if (one_block && slot == s->slot && lnum == s->lnum)
				leb_settle_target(
					volume, lnum,
					&old->volume[slot].leb[lnum],
					&s->after.volume[slot].leb[lnum],
					s->partial, leb);
			else
				leb_settle(volume, lnum, leb);
		}
	}
}

void leb_settle_failed(const struct model_step *s)
{
	const struct model_volume *volume = &model.volume[s->slot];
	const struct model_leb *old = &before.volume[s->slot].leb[s->lnum];
	struct model_leb *leb = &model.volume[s->slot].leb[s->lnum];
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_get_info(ubi, volume->vol_id, s->lnum, &info));

	if (!info.mapped) {
		zassert_true(!old->mapped || OP_ERASE == s->op,
			     "step %u: a failed %s unmapped volume %u block %u",
			     step, op_name[s->op], volume->vol_id, s->lnum);

		/* Unmapped before the erase failed, and may come back. */
		if (old->mapped) {
			leb->mapped = false;
			leb->ghost = true;
		}

		return;
	}

	const bool holds_old = leb_reads(volume, s->lnum, old);

	if (holds_old)
		return;

	zassert_true(
		OP_APPEND == s->op &&
			leb_reads_cut(volume, s->lnum,
				      &s->after.volume[s->slot].leb[s->lnum],
				      leb),
		"step %u: a failed %s left volume %u block %u neither as "
		"it was nor part way",
		step, op_name[s->op], volume->vol_id, s->lnum);

	stats.partial += 1;
}

void reattach(void)
{
	zassert_ok(ubi_device_deinit(ubi));
	attach_timed();

	zassert_true(volumes_are(&model),
		     "step %u: the volumes changed across a reattach", step);

	for (uint32_t slot = 0; slot < MODEL_VOLUMES; ++slot) {
		struct model_volume *volume = &model.volume[slot];

		if (!volume->exists)
			continue;

		for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum)
			leb_settle(volume, lnum, &volume->leb[lnum]);
	}
}
