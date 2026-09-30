/**
 * \file    ubi_maintenance.c
 * \author  Kamil Kielbasa
 * \brief   Work the device puts off until the application has time for it.
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
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_maintenance.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_relocate.h"
#include "ubi_volume.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Count the blocks waiting to be erased.
 */
static uint32_t reclaim_pending(const struct ubi_device *ubi);

/**
 * \brief Choose a block to erase.
 *
 *        Blocks a logical block let go of go first, since they still hold
 *        the application's data; of those, the copies a change left go
 *        before the blocks an unmap let go of.
 *
 * \retval 0
 *         Chosen.
 * \retval -ENOENT
 *         Nothing is waiting.
 */
static int reclaim_choose(const struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Erase and stamp one block.
 */
static int reclaim_step(struct ubi_device *ubi);

/**
 * \brief Count what is waiting to be put right.
 */
static uint32_t repair_pending(const struct ubi_device *ubi);

/**
 * \brief Find the first retired block.
 *
 * \retval 0
 *         Found.
 * \retval -ENOENT
 *         None is left.
 */
static int repair_choose(const struct ubi_device *ubi, uint32_t *pnum);

/**
 * \brief Put one thing right: the volume table first, then a retired block.
 */
static int repair_step(struct ubi_device *ubi);

/**
 * \brief Count the blocks kept as corrupt.
 */
static uint32_t discard_pending(const struct ubi_device *ubi);

/**
 * \brief Erase the first of them and put it back in service.
 */
static int discard_step(struct ubi_device *ubi);

/**
 * \brief Count the work of one kind that is waiting.
 */
static uint32_t maintenance_pending(const struct ubi_device *ubi,
				    enum ubi_maintenance_op operation);

/**
 * \brief Carry out one unit of work of one kind.
 */
static int maintenance_step(struct ubi_device *ubi,
			    enum ubi_maintenance_op operation);

/* Static function definitions --------------------------------------------- */

static uint32_t reclaim_pending(const struct ubi_device *ubi)
{
	uint32_t pending = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_RECLAIM == state || UBI_PEB_UNMAPPED == state ||
		    UBI_PEB_UNKNOWN == state)
			pending += 1;
	}

	return pending;
}

static int reclaim_choose(const struct ubi_device *ubi, uint32_t *pnum)
{
	static const enum ubi_peb_state order[] = {
		UBI_PEB_RECLAIM,
		UBI_PEB_UNMAPPED,
		UBI_PEB_UNKNOWN,
	};

	for (size_t i = 0; i < ARRAY_SIZE(order); ++i) {
		for (uint32_t candidate = 0;
		     candidate < ubi->geometry.peb_count; ++candidate) {
			const enum ubi_peb_state state =
				ubi_impl_peb_state_get(ubi, candidate);

			if (order[i] != state)
				continue;

			*pnum = candidate;

			return 0;
		}
	}

	return -ENOENT;
}

static int reclaim_step(struct ubi_device *ubi)
{
	uint32_t pnum = 0;
	const int ret = reclaim_choose(ubi, &pnum);

	if (0 != ret)
		return ret;

	return ubi_impl_peb_reclaim(ubi, pnum);
}

static uint32_t repair_pending(const struct ubi_device *ubi)
{
	uint32_t pending = ubi->volume_table.degraded ? 1 : 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_BAD == state)
			pending += 1;
	}

	return pending;
}

static int repair_choose(const struct ubi_device *ubi, uint32_t *pnum)
{
	for (uint32_t candidate = 0; candidate < ubi->geometry.peb_count;
	     ++candidate) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, candidate);

		if (UBI_PEB_BAD != state)
			continue;

		*pnum = candidate;

		return 0;
	}

	return -ENOENT;
}

static int repair_step(struct ubi_device *ubi)
{
	uint32_t pnum = 0;
	int ret = 0;

	if (ubi->volume_table.degraded)
		return ubi_impl_volumes_rewrite(ubi);

	ret = repair_choose(ubi, &pnum);

	if (0 != ret)
		return ret;

	/* Retirement lives in RAM, so a block put aside after one failure gets
	 * a second chance without waiting for the next attach. */
	ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_UNKNOWN);

	ret = ubi_impl_peb_prepare(ubi, pnum);

	/* The erase itself failed, which leaves the device read-only. */
	if (0 != ret && ubi->read_only) {
		ubi_impl_peb_state_set(ubi, pnum, UBI_PEB_BAD);
		return ret;
	}

	/* Written off, so that the next step reaches the blocks that may still
	 * come back. */
	if (0 != ret)
		ubi_impl_peb_write_off(ubi, pnum);

	return 0;
}

static uint32_t discard_pending(const struct ubi_device *ubi)
{
	uint32_t pending = 0;

	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_CORRUPT == state)
			pending += 1;
	}

	return pending;
}

static int discard_step(struct ubi_device *ubi)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		const enum ubi_peb_state state =
			ubi_impl_peb_state_get(ubi, pnum);

		if (UBI_PEB_CORRUPT != state)
			continue;

		const int ret = ubi_impl_peb_prepare(ubi, pnum);

		if (0 != ret)
			ubi_impl_peb_retire(ubi, pnum, UBI_VOL_ID_INVALID, 0);

		return ret;
	}

	return -ENOENT;
}

static uint32_t maintenance_pending(const struct ubi_device *ubi,
				    enum ubi_maintenance_op operation)
{
	switch (operation) {
	case UBI_MAINTENANCE_RECLAIM:
		return reclaim_pending(ubi);
	case UBI_MAINTENANCE_RELOCATE:
		return ubi_impl_relocate_pending(ubi);
	case UBI_MAINTENANCE_REPAIR:
		return repair_pending(ubi);
	case UBI_MAINTENANCE_DISCARD:
		return discard_pending(ubi);
	default:
		return 0;
	}
}

static int maintenance_step(struct ubi_device *ubi,
			    enum ubi_maintenance_op operation)
{
	switch (operation) {
	case UBI_MAINTENANCE_RECLAIM:
		return reclaim_step(ubi);
	case UBI_MAINTENANCE_RELOCATE:
		return ubi_impl_relocate_step(ubi);
	case UBI_MAINTENANCE_REPAIR:
		return repair_step(ubi);
	case UBI_MAINTENANCE_DISCARD:
		return discard_step(ubi);
	default:
		return -EINVAL;
	}
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_maintenance(struct ubi_device *ubi,
			 enum ubi_maintenance_op operation, uint32_t budget,
			 struct ubi_maintenance_result *result)
{
	uint32_t performed = 0;
	int ret = 0;

	/* A step can stand and still leave the device read-only. */
	while (performed < budget && !ubi->read_only) {
		ret = maintenance_step(ubi, operation);

		if (0 != ret)
			break;

		performed += 1;
	}

	result->performed = performed;
	result->remaining = maintenance_pending(ubi, operation);

	/* Running out of work is how a budget larger than the work ends. */
	if (0 != ret && -ENOENT != ret) {
		LOG_ERR("maintenance operation %d stopped after %u of %u "
			"steps (%d)",
			operation, performed, budget, ret);
		return ret;
	}

	return 0;
}

void ubi_impl_maintenance_report(const struct ubi_device *ubi,
				 enum ubi_maintenance_op operation,
				 struct ubi_maintenance_result *result)
{
	result->performed = 0;
	result->remaining = maintenance_pending(ubi, operation);
}
