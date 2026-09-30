/**
 * \file    flash_shim.c
 * \author  Kamil Kielbasa
 * \brief   A flash device in front of the simulator that can refuse reads.
 *
 *          The simulator fails writes and erases through its callbacks but
 *          has no way to fail a read. This device forwards everything to the
 *          simulator, fails reads of one chosen block, and the flash map
 *          below puts the partition under test behind it.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

/* Zephyr headers: */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

/* Test headers: */
#include "flash_shim.h"
#include "partition.h"

/* Module defines ---------------------------------------------------------- */

/** The simulator the partition really lives on. */
#define SHIM_BACKING \
	DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(UBI_TEST_PARTITION_NODE))

/** Where the partition under test starts on that device. */
#define SHIM_PARTITION_OFFSET DT_REG_ADDR(UBI_TEST_PARTITION_NODE)

/** Its size. */
#define SHIM_PARTITION_SIZE DT_REG_SIZE(UBI_TEST_PARTITION_NODE)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Forward a read, unless it touches the block whose reads fail.
 */
static int shim_read(const struct device *dev, off_t offset, void *data,
		     size_t len);

/**
 * \brief Forward a write.
 */
static int shim_write(const struct device *dev, off_t offset, const void *data,
		      size_t len);

/**
 * \brief Forward an erase.
 */
static int shim_erase(const struct device *dev, off_t offset, size_t size);

/**
 * \brief Report the simulator's parameters as this device's own.
 */
static const struct flash_parameters *
shim_get_parameters(const struct device *dev);

/**
 * \brief Report the simulator's size as this device's own.
 */
static int shim_get_size(const struct device *dev, uint64_t *size);

#if defined(CONFIG_FLASH_PAGE_LAYOUT)

/**
 * \brief Report the simulator's page layout as this device's own.
 */
static void shim_page_layout(const struct device *dev,
			     const struct flash_pages_layout **layout,
			     size_t *layout_size);

#endif /* CONFIG_FLASH_PAGE_LAYOUT */

/* Module variables and constants ------------------------------------------ */

/** Whether reads of \ref reads_fail_from to \ref reads_fail_to fail. */
static bool reads_fail = false;

/** First byte, within the partition, whose reads fail. */
static off_t reads_fail_from = 0;

/** One past the last. */
static off_t reads_fail_to = 0;

static DEVICE_API(flash, shim_api) = {
	.read = shim_read,
	.write = shim_write,
	.erase = shim_erase,
	.get_parameters = shim_get_parameters,
	.get_size = shim_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = shim_page_layout,
#endif
};

DEVICE_DEFINE(ubi_test_flash_shim, "ubi_test_flash_shim", NULL, NULL, NULL,
	      NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &shim_api);

/** The partition under test, and one that starts off a block boundary. */
static const struct flash_area shim_flash_map[] = {
	{
		.fa_id = UBI_TEST_PARTITION_ID,
		.fa_dev = DEVICE_GET(ubi_test_flash_shim),
		.fa_off = SHIM_PARTITION_OFFSET,
		.fa_size = SHIM_PARTITION_SIZE,
	},
	{
		.fa_id = UBI_TEST_MISALIGNED_PARTITION_ID,
		.fa_dev = DEVICE_GET(ubi_test_flash_shim),
		.fa_off = SHIM_PARTITION_OFFSET + UBI_TEST_PEB_SIZE / 2,
		.fa_size = SHIM_PARTITION_SIZE - UBI_TEST_PEB_SIZE,
	},
};

/* Stands in for the flash map Zephyr would build from the devicetree. */
const struct flash_area *flash_map = shim_flash_map;
const int flash_map_entries = ARRAY_SIZE(shim_flash_map);

/* Static function definitions --------------------------------------------- */

static int shim_read(const struct device *dev, off_t offset, void *data,
		     size_t len)
{
	ARG_UNUSED(dev);

	const off_t from = SHIM_PARTITION_OFFSET + reads_fail_from;
	const off_t to = SHIM_PARTITION_OFFSET + reads_fail_to;

	if (reads_fail && offset < to && offset + (off_t)len > from)
		return -EIO;

	return flash_read(SHIM_BACKING, offset, data, len);
}

static int shim_write(const struct device *dev, off_t offset, const void *data,
		      size_t len)
{
	ARG_UNUSED(dev);

	return flash_write(SHIM_BACKING, offset, data, len);
}

static int shim_erase(const struct device *dev, off_t offset, size_t size)
{
	ARG_UNUSED(dev);

	return flash_erase(SHIM_BACKING, offset, size);
}

static const struct flash_parameters *
shim_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return flash_get_parameters(SHIM_BACKING);
}

static int shim_get_size(const struct device *dev, uint64_t *size)
{
	ARG_UNUSED(dev);

	return flash_get_size(SHIM_BACKING, size);
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)

static void shim_page_layout(const struct device *dev,
			     const struct flash_pages_layout **layout,
			     size_t *layout_size)
{
	ARG_UNUSED(dev);

	const struct flash_driver_api *api = SHIM_BACKING->api;

	api->page_layout(SHIM_BACKING, layout, layout_size);
}

#endif /* CONFIG_FLASH_PAGE_LAYOUT */

/* Module interface function definitions ----------------------------------- */

void flash_fail_reads_in(uint32_t pnum, uint32_t from, uint32_t to)
{
	const off_t block = (off_t)pnum * UBI_TEST_PEB_SIZE;

	reads_fail_from = block + (off_t)from;
	reads_fail_to = block + (off_t)to;
	reads_fail = true;
}

void flash_fail_reads_of(uint32_t pnum)
{
	flash_fail_reads_in(pnum, 0, UBI_TEST_PEB_SIZE);
}

void flash_fail_reads_never(void)
{
	reads_fail = false;
}

const struct device *flash_simulator_device(void)
{
	return SHIM_BACKING;
}
