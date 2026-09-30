/**
 * \file    flash_faults.h
 * \author  Kamil Kielbasa
 * \brief   Writes and erases on the flash simulator made to fail, tear or
 *          cut the power, and writes over programmed bytes counted.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef FLASH_FAULTS_H
#define FLASH_FAULTS_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Function declarations --------------------------------------------------- */

/**
 * \brief Take over the simulator's writes and erases for the rest of the run.
 */
void flash_faults_install(void);

/**
 * \brief Disarm every fault and restore the power. Double writes seen so far
 *        stay counted.
 */
void flash_faults_clear(void);

/**
 * \brief Say whether the test itself is damaging the flash, so that its
 *        writes over programmed bytes are not counted.
 */
void flash_faults_damaging(bool damaging);

/**
 * \brief Forget the double writes seen so far.
 */
void flash_double_writes_forget(void);

/**
 * \brief Yield the CPU every \p bytes bytes written, so that another thread
 *        of the same priority runs in the middle of a flash operation. Zero
 *        stops it.
 */
void flash_yield_every(uint32_t bytes);

/**
 * \brief Fail every write over bytes that are not erased, as flash with ECC
 *        does, until the faults are cleared.
 */
void flash_refuse_overwrites(void);

/**
 * \brief Fail every flash write once \p after bytes have gone through.
 */
void flash_fail_writes_after(uint32_t after);

/**
 * \brief Fail the one write that reaches byte \p after, and none after it.
 */
void flash_fail_one_write_after(uint32_t after);

/**
 * \brief Let writes through again.
 */
void flash_fail_writes_never(void);

/**
 * \brief Fail every erase once \p after erases have gone through.
 */
void flash_fail_erases_after(uint32_t after);

/**
 * \brief Let erases through again.
 */
void flash_fail_erases_never(void);

/**
 * \brief Cut the power once \p after bytes have been written: the write in
 *        progress stops where it is and nothing reaches the flash after it.
 */
void flash_power_cut_after(uint32_t after);

/**
 * \brief Cut the power in the middle of the erase that follows \p after
 *        others. The erase gets as far as the second half of the block,
 *        which is where NOR flash may start.
 */
void flash_power_cut_during_erase(uint32_t after);

/**
 * \brief Report whether the power has been cut since it was last restored.
 */
bool flash_power_is_cut(void);

/**
 * \brief Report whether any injected failure has fired since the last clear.
 */
bool flash_fault_fired(void);

/**
 * \brief Writes the library made over bytes that were not erased, other than
 *        the zeroes that invalidate a header before an erase.
 */
uint32_t flash_double_writes(void);

/**
 * \brief Offset of the first of them within the partition.
 */
off_t flash_double_write_first(void);

/**
 * \brief Remember the whole partition, bypassing the flash driver.
 */
void flash_snapshot_take(void);

/**
 * \brief Put it back as remembered.
 */
void flash_snapshot_restore(void);

#endif /* FLASH_FAULTS_H */
