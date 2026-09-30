/**
 * \file    flash_shim.h
 * \author  Kamil Kielbasa
 * \brief   A flash device in front of the simulator that can refuse reads.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef FLASH_SHIM_H
#define FLASH_SHIM_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/device.h>

/* Function declarations --------------------------------------------------- */

/**
 * \brief Fail every read that touches bytes \p from to \p to - 1 of block
 *        \p pnum.
 */
void flash_fail_reads_in(uint32_t pnum, uint32_t from, uint32_t to);

/**
 * \brief Fail every read that touches block \p pnum.
 */
void flash_fail_reads_of(uint32_t pnum);

/**
 * \brief Let reads through again.
 */
void flash_fail_reads_never(void);

/**
 * \brief The simulator behind the partition under test.
 */
const struct device *flash_simulator_device(void);

#endif /* FLASH_SHIM_H */
