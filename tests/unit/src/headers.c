/**
 * \file    headers.c
 * \author  Kamil Kielbasa
 * \brief   Fixed header inputs and the byte-level helper the unit tests
 *          share.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Zephyr headers: */
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

/* UBI headers: */
#include "ubi_header.h"

/* Test headers: */
#include "headers.h"

/* Module variables and constants ------------------------------------------ */

const uint8_t erased_block[UBI_HEADER_SIZE] = {
	[0 ...(UBI_HEADER_SIZE - 1)] = 0xFF,
};

/* Module interface function definitions ----------------------------------- */

void fix_crc(uint8_t *buffer)
{
	sys_put_be32(crc32_ieee(buffer, HEADER_CRC_OFFSET),
		     &buffer[HEADER_CRC_OFFSET]);
}
