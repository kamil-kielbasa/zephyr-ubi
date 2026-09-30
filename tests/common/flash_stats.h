/**
 * \file    flash_stats.h
 * \author  Kamil Kielbasa
 * \brief   The flash simulator's operation counters.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef FLASH_STATS_H
#define FLASH_STATS_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Function declarations --------------------------------------------------- */

/**
 * \brief Read a flash simulator counter, such as \c "flash_erase_calls".
 */
uint32_t flash_ops(const char *name);

/**
 * \brief Erases the flash simulator counted on one block of the partition.
 */
uint32_t flash_erases_of(uint32_t pnum);

/**
 * \brief Put every flash simulator counter back to zero.
 */
void flash_ops_forget(void);

#endif /* FLASH_STATS_H */
