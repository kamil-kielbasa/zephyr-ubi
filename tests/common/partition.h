/**
 * \file    partition.h
 * \author  Kamil Kielbasa
 * \brief   The partition under test, read and damaged behind the library's
 *          back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef PARTITION_H
#define PARTITION_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* The one place the tests reach into the library: its on-flash layout. */
#include "ubi_header.h"
#include "ubi_private.h"

/* Defines ----------------------------------------------------------------- */

/** Partition under test; a board whose storage_partition is taken names one. */
#if DT_NODE_EXISTS(DT_NODELABEL(ubi_partition))
#define UBI_TEST_PARTITION_NODE DT_NODELABEL(ubi_partition)
#else
#define UBI_TEST_PARTITION_NODE DT_NODELABEL(storage_partition)
#endif

/** Flash map identifier of that partition. */
#define UBI_TEST_PARTITION_ID DT_FIXED_PARTITION_ID(UBI_TEST_PARTITION_NODE)

/** Flash memory the partition lives on, which carries its geometry. */
#define UBI_TEST_FLASH_NODE DT_GPARENT(UBI_TEST_PARTITION_NODE)

/* Checked against the driver by partition_geometry_check(). */
#if DT_NODE_HAS_COMPAT(UBI_TEST_FLASH_NODE, nordic_qspi_nor)
/* nrf_qspi_nor.c pages by Kconfig and writes whole words. */
#define UBI_TEST_PEB_SIZE CONFIG_NORDIC_QSPI_NOR_FLASH_LAYOUT_PAGE_SIZE
#define UBI_TEST_WRITE_BLOCK (4)
#else
#define UBI_TEST_PEB_SIZE DT_PROP(UBI_TEST_FLASH_NODE, erase_block_size)
#define UBI_TEST_WRITE_BLOCK DT_PROP(UBI_TEST_FLASH_NODE, write_block_size)
#endif

/** Physical erase blocks in the partition. */
#define UBI_TEST_PEB_COUNT \
	(DT_REG_SIZE(UBI_TEST_PARTITION_NODE) / UBI_TEST_PEB_SIZE)

/** What an erase leaves in every byte, on every flash these tests run on. */
#define UBI_TEST_ERASED (0xFF)

/** Flash map identifier of a partition starting half a block into the one
 *  under test, which no flash can erase in whole blocks. Simulator only. */
#define UBI_TEST_MISALIGNED_PARTITION_ID (0xF0)

/* Variable declarations --------------------------------------------------- */

/** Room for one block, shared by the helpers that rewrite a block whole. */
extern uint8_t block_scratch[UBI_TEST_PEB_SIZE];

/** Room for two blocks a test copies out and puts back later. */
extern uint8_t saved_blocks[2][UBI_TEST_PEB_SIZE];

/* Function declarations --------------------------------------------------- */

/**
 * \brief Fail the suite unless the driver reports the geometry above.
 */
void partition_geometry_check(void);

/**
 * \brief Overwrite the whole partition with one byte value.
 */
void partition_fill(uint8_t value);

/**
 * \brief Erase every block that is not blank.
 */
void partition_erase_dirty(void);

/**
 * \brief CRC of the whole partition, so a test can prove nothing moved.
 */
uint32_t partition_fingerprint(void);

/**
 * \brief Copy one block of the partition out, byte for byte.
 */
void block_save(uint32_t pnum, uint8_t *buffer);

/**
 * \brief Erase one block and write back what block_save() copied out.
 */
void block_restore(uint32_t pnum, const uint8_t *buffer);

/**
 * \brief Clear the lowest set bit of one byte, as NOR allows without an erase.
 *
 * \param[in] flash_area                Open partition.
 * \param at                            Offset of the byte to damage.
 */
void flash_clear_a_bit(const struct flash_area *flash_area, off_t at);

/**
 * \brief Clear a bit in the first byte of a range that still has one.
 *
 * \return 1 when a byte was damaged, 0 when every byte was already zero.
 */
uint32_t corrupt_a_byte_at(const struct flash_area *flash_area, off_t at,
			   size_t length);

/**
 * \brief Clear a bit in the data of every block whose data starts with
 *        \p needle.
 *
 * \return How many blocks were damaged.
 */
uint32_t corrupt_data_matching(const uint8_t *needle, size_t length);

/**
 * \brief Clear a bit in the volume identifier header of every block whose
 *        data starts with \p needle.
 *
 * \return How many headers were damaged.
 */
uint32_t corrupt_header_of_data_matching(const uint8_t *needle, size_t length);

/**
 * \brief Count the blocks whose data starts with \p needle.
 */
uint32_t count_data_matching(const uint8_t *needle, size_t length);

/**
 * \brief Number of the one block whose data starts with \p needle.
 */
uint32_t pnum_of_data_matching(const uint8_t *needle, size_t length);

#endif /* PARTITION_H */
