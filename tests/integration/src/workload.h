/**
 * \file    workload.h
 * \author  Kamil Kielbasa
 * \brief   Devices set up with work for maintenance: wear to level, and
 *          blocks to bring back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef WORKLOAD_H
#define WORKLOAD_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Function declarations --------------------------------------------------- */

/**
 * \brief Create and remove a volume a few times, so that the volume table
 *        copies stop being as little worn as a block written once.
 */
void volume_table_age(void);

/**
 * \brief Rewrite logical block zero until the cold blocks are worth
 *        relocating.
 *
 *        Every other block is mapped, so the churn stays on one pair of
 *        physical blocks.
 */
void wear_out_one_block(uint32_t vol_id);

/**
 * \brief Fill every logical block with \p cold, wear block zero out, and give
 *        the last block back so relocation has a choice to make.
 *
 * \param[in] cold                      Bytes every cold block holds.
 * \param length                        How many.
 * \param[out] leb_count                Logical blocks the volume holds.
 *
 * \return Identifier the volume was given.
 */
uint32_t unevenly_worn_device(const uint8_t *cold, size_t length,
			      uint32_t *leb_count);

/**
 * \brief Highest erase count behind logical blocks 1 to \p leb_count - 1.
 */
uint32_t cold_wear_max(uint32_t vol_id, uint32_t leb_count);

/**
 * \brief Fail unless logical blocks 1 to \p leb_count - 2 hold \p cold.
 *
 *        The last one was given back by unevenly_worn_device().
 */
void cold_blocks_check(uint32_t vol_id, uint32_t leb_count, const uint8_t *cold,
		       size_t length);

/**
 * \brief Retire one block by refusing the write that would have landed on it.
 *        Flash simulator only.
 */
void retire_one_block(uint32_t vol_id, uint32_t lnum);

#endif /* WORKLOAD_H */
