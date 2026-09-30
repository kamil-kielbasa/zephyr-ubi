/**
 * \file    model_step.c
 * \author  Kamil Kielbasa
 * \brief   Picking one operation, carrying it out, and holding the device to
 *          the model afterwards.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "model.h"
#include "model_step.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

#if defined(CONFIG_FLASH_SIMULATOR)

/** Bytes an operation writes that a power cut is placed among. */
#define CUT_REACH (1536)

/** Erases an operation starts that a torn erase is placed among. */
#define ERASE_CUT_REACH (4)

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module type definitions ------------------------------------------------- */

/** How an operation is cut short. */
enum model_fault {
	FAULT_NONE,
	FAULT_POWER_CUT,
	FAULT_ERASE_CUT,
	FAULT_WRITE_FAILS,
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Report whether blocks out of service explain a lack of space.
 */
static bool space_explained(void);

/**
 * \brief Pick a volume and a logical block of it.
 */
static bool leb_pick(uint32_t *slot, uint32_t *lnum);

/**
 * \brief Pick an operation, from those that write when \p writing.
 */
static enum model_op op_pick(bool writing);

/**
 * \brief Choose the arguments of \p op and work out what it leaves.
 *
 * \return Whether the operation can be carried out at all.
 */
static bool step_plan(enum model_op op, struct model_step *s);

/**
 * \brief Carry an operation out.
 */
static int step_perform(const struct model_step *s);

/**
 * \brief Carry an operation out, cut short as \p fault says, and check what
 *        the device holds afterwards.
 */
static void step_execute(struct model_step *s, enum model_fault fault);

/* Module variables and constants ------------------------------------------ */

/** The operation under way. */
static struct model_step current = { 0 };

/** Bytes a write takes from. */
static uint8_t buffer[MODEL_WRITE_MAX] = { 0 };

/** How often each operation is picked. */
static const uint8_t op_weight[OP_COUNT] = {
	[OP_CHANGE] = 20,   [OP_APPEND] = 12,  [OP_READ] = 8,
	[OP_MAP] = 4,	    [OP_UNMAP] = 6,    [OP_ERASE] = 5,
	[OP_CREATE] = 4,    [OP_REMOVE] = 2,   [OP_RESIZE] = 3,
	[OP_MAINTAIN] = 14, [OP_REATTACH] = 2,
};

/* Static function definitions --------------------------------------------- */

static bool space_explained(void)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	return 0 != info.bad_pebs + info.corrupt_pebs;
}

static bool leb_pick(uint32_t *slot, uint32_t *lnum)
{
	const uint32_t first = random_below(MODEL_VOLUMES);

	for (uint32_t i = 0; i < MODEL_VOLUMES; ++i) {
		const uint32_t candidate = (first + i) % MODEL_VOLUMES;
		const struct model_volume *volume = &model.volume[candidate];

		if (!volume->exists)
			continue;

		*slot = candidate;
		*lnum = random_below(volume->leb_count);

		return true;
	}

	return false;
}

static enum model_op op_pick(bool writing)
{
	uint32_t total = 0;

	for (uint32_t op = 0; op < OP_COUNT; ++op)
		total += op_weight[op];

	for (;;) {
		uint32_t pick = random_below(total);
		uint32_t op = 0;

		while (pick >= op_weight[op]) {
			pick -= op_weight[op];
			op += 1;
		}

		/* None of these writes anything a fault could cut short. */
		if (writing &&
		    (OP_READ == op || OP_UNMAP == op || OP_REATTACH == op))
			continue;

		return (enum model_op)op;
	}
}

static bool step_plan(enum model_op op, struct model_step *s)
{
	*s = (struct model_step){ .op = op, .after = model };

	if (OP_CREATE == op) {
		struct ubi_device_info info = { 0 };

		zassert_ok(ubi_device_get_info(ubi, &info));

		for (s->slot = 0; s->slot < MODEL_VOLUMES; ++s->slot) {
			if (!model.volume[s->slot].exists)
				break;
		}

		if (MODEL_VOLUMES == s->slot || 0 == info.free_lebs)
			return false;

		s->leb_count =
			1 + random_below(MIN(MODEL_LEBS, info.free_lebs));
		s->after.volume[s->slot] = (struct model_volume){
			.exists = true,
			.vol_id = UBI_VOL_ID_INVALID,
			.leb_count = s->leb_count,
		};

		return true;
	}

	if (OP_MAINTAIN == op) {
		const uint32_t maintenance =
			random_below(UBI_MAINTENANCE_DISCARD + 1);

		s->maintenance = (enum ubi_maintenance_op)maintenance;
		s->budget = 1 + random_below(4);

		return true;
	}

	if (OP_REATTACH == op)
		return true;

	const bool picked = leb_pick(&s->slot, &s->lnum);

	if (!picked)
		return false;

	struct model_volume *volume = &s->after.volume[s->slot];
	struct model_leb *leb = &volume->leb[s->lnum];
	const uint32_t writes = MODEL_WRITE_MAX / write_block;

	s->length = write_block * (1 + random_below(writes));
	s->seed = random_next();

	switch (op) {
	case OP_CHANGE:
		/* With nothing older behind it, a copy cut short is kept. */
		s->partial = !leb->mapped;
		*leb = (struct model_leb){
			.mapped = true,
			.segments = 1,
			.segment = { { .seed = s->seed, .length = s->length } },
		};
		break;
	case OP_APPEND:
		if (leb->mapped &&
		    (leb->closed || MODEL_SEGMENTS == leb->segments))
			return false;

		if (!leb->mapped)
			*leb = (struct model_leb){ .mapped = true };

		s->offset = leb_written(leb);
		s->partial = true;
		leb->segment[leb->segments].seed = s->seed;
		leb->segment[leb->segments].length = s->length;
		leb->segments += 1;
		break;
	case OP_MAP:
		if (leb->mapped) {
			s->expected = -EEXIST;
			break;
		}

		*leb = (struct model_leb){ .mapped = true };
		break;
	case OP_UNMAP:
		if (leb->mapped) {
			leb->mapped = false;
			leb->ghost = true;
		}
		break;
	case OP_ERASE:
		*leb = (struct model_leb){ 0 };
		break;
	case OP_REMOVE:
		*volume = (struct model_volume){ 0 };
		break;
	case OP_RESIZE: {
		struct ubi_device_info info = { 0 };

		zassert_ok(ubi_device_get_info(ubi, &info));

		s->leb_count = 1 + random_below(MODEL_LEBS);

		if (s->leb_count > volume->leb_count + info.free_lebs)
			s->leb_count = volume->leb_count + info.free_lebs;

		for (uint32_t lnum = s->leb_count; lnum < volume->leb_count;
		     ++lnum) {
			if (volume->leb[lnum].mapped)
				s->expected = -EBUSY;
		}

		if (0 != s->expected)
			break;

		/* Shrunk away or grown back, a block starts empty. */
		for (uint32_t lnum = MIN(s->leb_count, volume->leb_count);
		     lnum < MODEL_LEBS; ++lnum)
			volume->leb[lnum] = (struct model_leb){ 0 };

		volume->leb_count = s->leb_count;
		break;
	}
	case OP_READ:
	case OP_CREATE:
	case OP_MAINTAIN:
	case OP_REATTACH:
	case OP_COUNT:
	default:
		break;
	}

	if (0 != s->expected)
		s->after = model;

	return true;
}

static int step_perform(const struct model_step *s)
{
	const struct model_volume *volume = &model.volume[s->slot];
	struct ubi_maintenance_result result = { 0 };
	char name[MODEL_NAME_SIZE] = { 0 };
	int ret = 0;

	for (uint32_t i = 0; i < s->length; ++i)
		buffer[i] = segment_byte(s->seed, i);

	switch (s->op) {
	case OP_CHANGE:
		return ubi_leb_change(ubi, volume->vol_id, s->lnum, buffer,
				      s->length);
	case OP_APPEND:
		return ubi_leb_write_at(ubi, volume->vol_id, s->lnum, s->offset,
					buffer, s->length);
	case OP_MAP:
		return ubi_leb_map(ubi, volume->vol_id, s->lnum);
	case OP_UNMAP:
		return ubi_leb_unmap(ubi, volume->vol_id, s->lnum);
	case OP_ERASE:
		return ubi_leb_erase(ubi, volume->vol_id, s->lnum);
	case OP_CREATE: {
		const struct ubi_volume_config wanted = {
			.name = name,
			.leb_count = s->leb_count,
		};
		uint32_t vol_id = UBI_VOL_ID_INVALID;

		volume_name(s->slot, name);

		return ubi_volume_create(ubi, &wanted, &vol_id);
	}
	case OP_REMOVE:
		return ubi_volume_remove(ubi, volume->vol_id);
	case OP_RESIZE:
		return ubi_volume_resize(ubi, volume->vol_id, s->leb_count);
	case OP_MAINTAIN:
		ret = ubi_maintenance(ubi, s->maintenance, s->budget, &result);

		if (UBI_MAINTENANCE_RELOCATE == s->maintenance)
			stats.relocated += result.performed;

		return ret;
	case OP_READ:
	case OP_REATTACH:
	case OP_COUNT:
	default:
		return 0;
	}
}

static void step_execute(struct model_step *s, enum model_fault fault)
{
	const bool one_block = OP_CHANGE == s->op || OP_APPEND == s->op ||
			       OP_MAP == s->op || OP_ERASE == s->op;
	bool cut = false;
	bool fired = false;

	before = model;

#if defined(CONFIG_FLASH_SIMULATOR)
	switch (fault) {
	case FAULT_POWER_CUT:
		flash_power_cut_after(random_below(CUT_REACH));
		break;
	case FAULT_ERASE_CUT:
		flash_power_cut_during_erase(random_below(ERASE_CUT_REACH));
		break;
	case FAULT_WRITE_FAILS:
		flash_fail_one_write_after(random_below(CUT_REACH));
		break;
	case FAULT_NONE:
	default:
		break;
	}
#endif

	const int ret = step_perform(s);

#if defined(CONFIG_FLASH_SIMULATOR)
	cut = flash_power_is_cut();
	fired = flash_fault_fired();
	flash_faults_clear();
#else
	ARG_UNUSED(fault);
#endif

	if (cut) {
		stats.cuts += 1;
		zassert_ok(ubi_device_deinit(ubi));
		attach_timed();

		const bool took = volumes_are(&s->after);

		if (took) {
			model = s->after;
		} else {
			model = before;
			zassert_true(
				volumes_are(&model),
				"step %u: a cut %s left volumes neither as "
				"before nor as after it",
				step, op_name[s->op]);
		}

		model_settle(&model, s, &before);
		return;
	}

	if (fired) {
		stats.failures += 1;

		/* What the device serves now is what the next attach finds. */
		if (0 == ret) {
			model = s->after;
		} else {
			model = before;

			if (one_block)
				leb_settle_failed(s);
		}

		model_verify();
		return;
	}

	const bool no_space = -ENOSPC == ret && -ENOSPC != s->expected &&
			      space_explained();

	if (no_space) {
		stats.no_space += 1;
		model_verify();
		return;
	}

	zassert_equal(s->expected, ret, "step %u: %s returned %d, not %d", step,
		      op_name[s->op], ret, s->expected);

	if (0 != s->expected)
		stats.refused += 1;

	model = s->after;

	if (one_block || OP_UNMAP == s->op || OP_READ == s->op)
		zassert_true(leb_reads(&model.volume[s->slot], s->lnum,
				       &model.volume[s->slot].leb[s->lnum]),
			     "step %u: after %s volume %u block %u does not "
			     "hold what the model has",
			     step, op_name[s->op], model.volume[s->slot].vol_id,
			     s->lnum);
	else
		model_verify();
}

/* Module interface function definitions ----------------------------------- */

void step_run(void)
{
	enum model_fault fault = FAULT_NONE;

#if defined(CONFIG_FLASH_SIMULATOR)
	const bool faulty = random_below(100) <
			    CONFIG_UBI_TEST_MODEL_FAULT_PERCENT;

	if (faulty)
		fault = (enum model_fault)(1 + random_below(3));
#endif

	for (;;) {
		const enum model_op op = op_pick(FAULT_NONE != fault);
		const bool planned = step_plan(op, &current);

		if (!planned)
			continue;

		stats.done[op] += 1;

		if (OP_REATTACH == op)
			reattach();
		else
			step_execute(&current, fault);

		return;
	}
}
