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
#include <stddef.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Defines ----------------------------------------------------------------- */

/** The partition every test formats, fills and inspects. */
#define TEST_PARTITION FIXED_PARTITION_ID(storage_partition)

/** Where a block's data begins, behind its two 64-byte headers. */
#define UBI_TEST_DATA_OFFSET (128)

/** Geometry of the flash this build runs on, straight from the overlay. */
#define UBI_TEST_PEB_SIZE DT_PROP(DT_NODELABEL(flash0), erase_block_size)
#define UBI_TEST_WRITE_BLOCK DT_PROP(DT_NODELABEL(flash0), write_block_size)

/* Function declarations --------------------------------------------------- */

/**
 * \brief Overwrite the whole partition with one byte value.
 */
void partition_fill(uint8_t value);

/**
 * \brief Clear the lowest set bit of one byte on the flash.
 *
 *        The write goes out a whole write block at a time, because that is
 *        the smallest unit a real NOR part accepts, and the surrounding
 *        bytes go back unchanged.
 *
 * \param[in] flash_area                Open partition.
 * \param at                            Offset of the byte to damage.
 */
void flash_clear_a_bit(const struct flash_area *flash_area, off_t at);

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

/**
 * \brief Find a block whose data area starts with \p needle and flip a bit.
 *
 *        Stands in for a power loss between a sealed header and the last
 *        byte of the data it promises.
 *
 * \param[in] needle                    Bytes to look for.
 * \param length                        How many.
 *
 * \return How many blocks were changed.
 */
uint32_t corrupt_data_matching(const uint8_t *needle, size_t length);

/**
 * \brief Count the blocks whose data area starts with \p needle.
 *
 * \param[in] needle                    Bytes to look for.
 * \param length                        How many.
 *
 * \return How many blocks carry them.
 */
uint32_t count_data_matching(const uint8_t *needle, size_t length);

#endif /* COMMON_H */
