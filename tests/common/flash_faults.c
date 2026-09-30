/**
 * \file    flash_faults.c
 * \author  Kamil Kielbasa
 * \brief   Writes and erases on the flash simulator made to fail, tear or
 *          cut the power, and writes over programmed bytes counted.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* Test headers: */
#include "flash_faults.h"
#include "flash_shim.h"
#include "partition.h"

/* Module defines ---------------------------------------------------------- */

/** Where the partition under test starts on the simulator. */
#define SIMULATOR_PARTITION_OFFSET DT_REG_ADDR(UBI_TEST_PARTITION_NODE)

/** Bytes a header magic occupies, which is what an invalidation clears. */
#define HEADER_MAGIC_SIZE (4)

/** Bytes an invalidation writes: the magic, padded to a write block. */
#define HEADER_INVALIDATION_SIZE MAX(HEADER_MAGIC_SIZE, UBI_TEST_WRITE_BLOCK)

/* Module type definitions ------------------------------------------------- */

/** Everything the simulator is currently told to get wrong. */
struct flash_faults {
	/** A write is going to fail. */
	bool writes_fail;
	/** Bytes let through before it does. */
	uint32_t writes_left;
	/** Only that one write fails; the flash recovers after it. */
	bool writes_recover;
	/** An erase is going to fail. */
	bool erases_fail;
	/** Erases let through before it does. */
	uint32_t erases_left;
	/** The failure is a power cut: it tears what it hits and nothing gets
	 *  through after it. */
	bool power_cuts;
	/** The power is gone. */
	bool power_cut;
	/** An injected failure has been returned since the last clear. */
	bool fired;
	/** A test is damaging the flash on purpose. */
	bool damaging;
	/** Library writes over bytes that were not erased. */
	uint32_t double_writes;
	/** Where the first of them landed, within the partition. */
	off_t double_write_at;
	/** Bytes written between two yields of the CPU, or zero for none. */
	uint32_t yield_every;
	/** Bytes written since the hook was set. */
	uint32_t written;
	/** Writes over programmed bytes fail, as on flash with ECC. */
	bool overwrites_refused;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Report whether a write of \p data at \p at, within the partition,
 *        is the library clearing a header magic before an erase.
 */
static bool header_invalidation(off_t at, uint8_t data);

/**
 * \brief Count a write over a byte that is not erased, unless the test is
 *        doing the damage or the library is invalidating a header.
 */
static void double_write_note(off_t offset, uint8_t data);

/**
 * \brief Refuse a write once its allowance is spent, and watch for writes
 *        over bytes that were not erased.
 */
static int write_byte_fails(const struct device *dev, off_t offset,
			    uint8_t data);

/**
 * \brief Refuse an erase once its allowance is spent, tear it if that is a
 *        power cut, and carry it out by hand otherwise.
 */
static int erase_unit_fails(const struct device *dev, off_t unit_offset);

/* Module variables and constants ------------------------------------------ */

/** What the simulator is told to get wrong. */
static struct flash_faults faults = { 0 };

/** The simulator's memory, which its callbacks see before a byte lands. */
static uint8_t *simulator_memory = NULL;

/** The partition as flash_snapshot_take() found it. */
static uint8_t snapshot[UBI_TEST_PEB_COUNT * UBI_TEST_PEB_SIZE] = { 0 };

/** Callbacks the simulator runs for every byte written and block erased. */
static const struct flash_simulator_cb injected_faults = {
	.write_byte = write_byte_fails,
	.erase_unit = erase_unit_fails,
};

/* Static function definitions --------------------------------------------- */

static bool header_invalidation(off_t at, uint8_t data)
{
	const off_t within = at % UBI_TEST_PEB_SIZE;

	if (0x00 != data)
		return false;

	if (within < HEADER_INVALIDATION_SIZE)
		return true;

	return within >= UBI_VID_HEADER_OFFSET &&
	       within < UBI_VID_HEADER_OFFSET + HEADER_INVALIDATION_SIZE;
}

static void double_write_note(off_t offset, uint8_t data)
{
	const off_t at = offset - SIMULATOR_PARTITION_OFFSET;

	if (faults.damaging || UBI_TEST_ERASED == simulator_memory[offset])
		return;

	const bool invalidation = header_invalidation(at, data);

	if (invalidation)
		return;

	if (0 == faults.double_writes)
		faults.double_write_at = at;

	faults.double_writes += 1;
}

static int write_byte_fails(const struct device *dev, off_t offset,
			    uint8_t data)
{
	ARG_UNUSED(dev);

	if (faults.power_cut)
		return -EIO;

	if (faults.writes_fail && 0 == faults.writes_left) {
		faults.writes_fail = !faults.writes_recover;
		faults.power_cut = faults.power_cuts;
		faults.fired = true;
		return -EIO;
	}

	if (faults.writes_fail)
		faults.writes_left -= 1;

	if (faults.overwrites_refused && !faults.damaging &&
	    UBI_TEST_ERASED != simulator_memory[offset])
		return -EIO;

	double_write_note(offset, data);

	faults.written += 1;

	if (0 != faults.yield_every && 0 == faults.written % faults.yield_every)
		k_yield();

	return data;
}

static int erase_unit_fails(const struct device *dev, off_t unit_offset)
{
	size_t size = 0;
	uint8_t *memory = flash_simulator_get_memory(dev, &size);
	const uint8_t erased = flash_get_parameters(dev)->erase_value;

	zassert_not_null(memory, "the simulator has no memory to erase");
	zassert_true((size_t)unit_offset + UBI_TEST_PEB_SIZE <= size);

	if (faults.power_cut)
		return -EIO;

	if (faults.erases_fail && 0 == faults.erases_left) {
		if (faults.power_cuts) {
			memset(&memory[unit_offset + UBI_TEST_PEB_SIZE / 2],
			       erased, UBI_TEST_PEB_SIZE / 2);
			faults.power_cut = true;
		}

		faults.fired = true;

		return -EIO;
	}

	if (faults.erases_fail)
		faults.erases_left -= 1;

	/* The simulator stops erasing on its own once this callback exists. */
	memset(&memory[unit_offset], erased, UBI_TEST_PEB_SIZE);

	return 0;
}

/* Module interface function definitions ----------------------------------- */

void flash_faults_install(void)
{
	size_t size = 0;

	simulator_memory =
		flash_simulator_get_memory(flash_simulator_device(), &size);

	zassert_not_null(simulator_memory, "the simulator has no memory");
	zassert_true(SIMULATOR_PARTITION_OFFSET + sizeof(snapshot) <= size);

	flash_simulator_set_callbacks(flash_simulator_device(),
				      &injected_faults);
}

void flash_faults_clear(void)
{
	const uint32_t double_writes = faults.double_writes;
	const off_t double_write_at = faults.double_write_at;

	faults = (struct flash_faults){ 0 };
	faults.double_writes = double_writes;
	faults.double_write_at = double_write_at;

	flash_fail_reads_never();
}

void flash_faults_damaging(bool damaging)
{
	faults.damaging = damaging;
}

void flash_double_writes_forget(void)
{
	faults.double_writes = 0;
	faults.double_write_at = 0;
}

void flash_yield_every(uint32_t bytes)
{
	faults.yield_every = bytes;
	faults.written = 0;
}

void flash_refuse_overwrites(void)
{
	faults.overwrites_refused = true;
}

void flash_fail_writes_after(uint32_t after)
{
	faults.writes_left = after;
	faults.writes_fail = true;
	faults.writes_recover = false;
	faults.power_cuts = false;
}

void flash_fail_one_write_after(uint32_t after)
{
	flash_fail_writes_after(after);
	faults.writes_recover = true;
}

void flash_fail_writes_never(void)
{
	faults.writes_fail = false;
}

void flash_fail_erases_after(uint32_t after)
{
	faults.erases_left = after;
	faults.erases_fail = true;
	faults.power_cuts = false;
}

void flash_fail_erases_never(void)
{
	faults.erases_fail = false;
}

void flash_power_cut_after(uint32_t after)
{
	flash_fail_writes_after(after);
	faults.power_cuts = true;
	faults.power_cut = false;
}

void flash_power_cut_during_erase(uint32_t after)
{
	flash_fail_erases_after(after);
	faults.power_cuts = true;
	faults.power_cut = false;
}

bool flash_power_is_cut(void)
{
	return faults.power_cut;
}

bool flash_fault_fired(void)
{
	return faults.fired;
}

uint32_t flash_double_writes(void)
{
	return faults.double_writes;
}

off_t flash_double_write_first(void)
{
	return faults.double_write_at;
}

void flash_snapshot_take(void)
{
	memcpy(snapshot, &simulator_memory[SIMULATOR_PARTITION_OFFSET],
	       sizeof(snapshot));
}

void flash_snapshot_restore(void)
{
	memcpy(&simulator_memory[SIMULATOR_PARTITION_OFFSET], snapshot,
	       sizeof(snapshot));
}
