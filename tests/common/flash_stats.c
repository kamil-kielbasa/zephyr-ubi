/**
 * \file    flash_stats.c
 * \author  Kamil Kielbasa
 * \brief   The flash simulator's operation counters.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/stats/stats.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

/* Test headers: */
#include "flash_stats.h"
#include "partition.h"

/* Module defines ---------------------------------------------------------- */

/** Room for the longest name the simulator registers, "erase_cycles_unit255". */
#define COUNTER_NAME_SIZE (32)

/* Module type definitions ------------------------------------------------- */

/** One counter being looked for while walking the statistics group. */
struct counter_wanted {
	/** Name to match. */
	const char *name;
	/** What it held. */
	uint32_t value;
	/** Whether the name was found at all. */
	bool found;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Pick the wanted counter out of the statistics group being walked.
 */
static int counter_match(struct stats_hdr *group, void *arg, const char *name,
			 uint16_t offset);

/* Static function definitions --------------------------------------------- */

static int counter_match(struct stats_hdr *group, void *arg, const char *name,
			 uint16_t offset)
{
	struct counter_wanted *wanted = arg;
	const uint8_t *entries = (const uint8_t *)group;
	const bool other = (0 != strcmp(wanted->name, name));

	if (other)
		return 0;

	zassert_equal(sizeof(wanted->value), group->s_size,
		      "counter \"%s\" is not 32 bits wide", name);

	memcpy(&wanted->value, &entries[offset], sizeof(wanted->value));
	wanted->found = true;

	return 0;
}

/* Module interface function definitions ----------------------------------- */

uint32_t flash_ops(const char *name)
{
	struct stats_hdr *group = stats_group_find("flash_sim_stats");
	struct counter_wanted wanted = { .name = name,
					 .value = 0,
					 .found = false };

	zassert_not_null(group, "the flash simulator keeps no counters");
	zassert_ok(stats_walk(group, counter_match, &wanted));
	zassert_true(wanted.found, "there is no counter called \"%s\"", name);

	return wanted.value;
}

uint32_t flash_erases_of(uint32_t pnum)
{
	const struct flash_area *flash_area = NULL;
	char name[COUNTER_NAME_SIZE] = { 0 };

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	const uint32_t unit =
		(uint32_t)(flash_area->fa_off / UBI_TEST_PEB_SIZE) + pnum;

	flash_area_close(flash_area);

	const int length =
		snprintf(name, sizeof(name), "erase_cycles_unit%u", unit);

	zassert_true(0 < length);
	zassert_true((size_t)length < sizeof(name));

	return flash_ops(name);
}

void flash_ops_forget(void)
{
	struct stats_hdr *group = stats_group_find("flash_sim_stats");

	zassert_not_null(group, "the flash simulator keeps no counters");
	stats_reset(group);
}
