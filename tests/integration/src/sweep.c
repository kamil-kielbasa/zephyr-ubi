/**
 * \file    sweep.c
 * \author  Kamil Kielbasa
 * \brief   Running one operation over and over from the same partition, with
 *          a fault moved a little further along each time. Flash simulator
 *          only.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <string.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "flash_faults.h"
#include "suite.h"
#include "sweep.h"

/* Module interface function definitions ----------------------------------- */

void sweep_attach(void)
{
	zassert_ok(ubi_device_init(ubi, &config));
}

void sweep_reboot(void)
{
	flash_faults_clear();
	zassert_ok(ubi_device_deinit(ubi));
}

bool leb_holds(uint32_t vol_id, uint32_t lnum, const uint8_t *expected,
	       size_t length)
{
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	zassert_true(length <= sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, read, length));

	return 0 == memcmp(expected, read, length);
}

void leb_check(uint32_t vol_id, uint32_t lnum, const uint8_t *expected,
	       size_t length)
{
	zassert_true(leb_holds(vol_id, lnum, expected, length),
		     "volume %u block %u lost its contents", vol_id, lnum);
}

bool volume_present(const char *name)
{
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	return 0 == ubi_volume_find(ubi, name, &vol_id);
}
