/**
 * \file    common.h
 * \author  Kamil Kielbasa
 * \brief   Reaching the flash behind the library's back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef COMMON_H
#define COMMON_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Defines ----------------------------------------------------------------- */

/** The partition every test formats, fills and inspects. */
#define TEST_PARTITION FIXED_PARTITION_ID(storage_partition)

/* Function declarations --------------------------------------------------- */

/**
 * \brief Overwrite the whole partition with one byte value.
 */
void partition_fill(uint8_t value);

/**
 * \brief Fingerprint the partition, so a test can prove nothing moved.
 */
uint32_t partition_fingerprint(void);

/**
 * \brief Flip one byte in the first \p copies volume table records found.
 *
 * \param[in,out] ubi                   Handle to attach with, left detached.
 * \param[in] config                    Configuration that reads them.
 * \param copies                        How many to damage.
 *
 * \return How many records were reached.
 */
uint32_t corrupt_volume_tables(struct ubi_device *ubi,
			       const struct ubi_config *config,
			       uint32_t copies);

#endif /* COMMON_H */
