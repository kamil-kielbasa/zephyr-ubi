/**
 * \file    ubi_io.c
 * \author  Kamil Kielbasa
 * \brief   Physical access to the managed partition.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

/* UBI headers: */
#include "ubi_io.h"
#include "ubi_private.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Where a block, and an offset within it, sit in the partition.
 *
 *        The arguments are the caller's to check; by here they are known
 *        good.
 */
static off_t io_offset(const struct ubi_device *ubi, uint32_t pnum,
		       uint32_t offset);

/* Static function definitions --------------------------------------------- */

static off_t io_offset(const struct ubi_device *ubi, uint32_t pnum,
		       uint32_t offset)
{
	return (off_t)pnum * (off_t)ubi->geometry.peb_size + (off_t)offset;
}

/* Module interface function definitions ----------------------------------- */

int ubi_io_read(const struct ubi_device *ubi, uint32_t pnum, uint32_t offset,
		uint8_t *buffer, size_t length)
{
	if (NULL == ubi || NULL == ubi->flash_area || NULL == buffer ||
	    0 == length) {
		LOG_ERR("PEB %u: a read needs an open partition and somewhere "
			"to put %zu bytes",
			pnum, length);
		return -EINVAL;
	}

	if (pnum >= ubi->geometry.peb_count ||
	    offset >= ubi->geometry.peb_size ||
	    length > ubi->geometry.peb_size - offset) {
		LOG_ERR("PEB %u: a read of %zu bytes at %u leaves a partition "
			"of %u blocks of %u bytes",
			pnum, length, offset, ubi->geometry.peb_count,
			ubi->geometry.peb_size);
		return -EINVAL;
	}

	const int ret = flash_area_read(
		ubi->flash_area, io_offset(ubi, pnum, offset), buffer, length);

	if (0 != ret) {
		LOG_ERR("PEB %u: reading %zu bytes at %u failed (%d)", pnum,
			length, offset, ret);
		return -EIO;
	}

	return 0;
}

int ubi_io_write(const struct ubi_device *ubi, uint32_t pnum, uint32_t offset,
		 const uint8_t *buffer, size_t length)
{
	if (NULL == ubi || NULL == ubi->flash_area || NULL == buffer ||
	    0 == length) {
		LOG_ERR("PEB %u: a write needs an open partition and %zu bytes "
			"to write",
			pnum, length);
		return -EINVAL;
	}

	if (pnum >= ubi->geometry.peb_count ||
	    offset >= ubi->geometry.peb_size ||
	    length > ubi->geometry.peb_size - offset) {
		LOG_ERR("PEB %u: a write of %zu bytes at %u leaves a partition "
			"of %u blocks of %u bytes",
			pnum, length, offset, ubi->geometry.peb_count,
			ubi->geometry.peb_size);
		return -EINVAL;
	}

	const int ret = flash_area_write(
		ubi->flash_area, io_offset(ubi, pnum, offset), buffer, length);

	if (0 != ret) {
		LOG_ERR("PEB %u: writing %zu bytes at %u failed (%d)", pnum,
			length, offset, ret);
		return -EIO;
	}

	return 0;
}

int ubi_io_erase(const struct ubi_device *ubi, uint32_t pnum)
{
	if (NULL == ubi || NULL == ubi->flash_area) {
		LOG_ERR("PEB %u: an erase needs an open partition", pnum);
		return -EINVAL;
	}

	if (0 == ubi->geometry.peb_size || pnum >= ubi->geometry.peb_count) {
		LOG_ERR("PEB %u is not one of the %u blocks this partition "
			"holds",
			pnum, ubi->geometry.peb_count);
		return -EINVAL;
	}

	const int ret = flash_area_erase(ubi->flash_area,
					 io_offset(ubi, pnum, 0),
					 ubi->geometry.peb_size);

	if (0 != ret) {
		LOG_ERR("PEB %u: erase failed (%d)", pnum, ret);
		return -EIO;
	}

	return 0;
}
