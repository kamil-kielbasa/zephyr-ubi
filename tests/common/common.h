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

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* The one place the suite reaches into the library: its on-flash layout. */
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

/* Checked against the driver in suite_setup(). */
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
 * \brief Erase a block and give it an authentic erase counter header.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param pnum                          Block to restamp.
 * \param image_seq                     Image the block claims.
 * \param erase_count                   Count to write into the header.
 */
void stamp_erase_count(psa_key_id_t ikm_key_id, uint32_t pnum,
		       uint32_t image_seq, uint64_t erase_count);

/**
 * \brief Read the erase counter header of every block straight off the flash.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param[out] counts                   One count per block.
 * \param peb_count                     How many blocks to read.
 */
void erase_counts_on_flash(psa_key_id_t ikm_key_id, uint64_t *counts,
			   uint32_t peb_count);

/**
 * \brief Clear the lowest set bit of one byte, as NOR allows without an erase.
 *
 * \param[in] flash_area                Open partition.
 * \param at                            Offset of the byte to damage.
 */
void flash_clear_a_bit(const struct flash_area *flash_area, off_t at);

/**
 * \brief Change one byte of a header and repair its checksum, so only the
 *        MAC can object.
 *
 * \param pnum                          Block to rewrite.
 * \param at                            Offset of the byte within the block.
 */
void forge_header_byte(uint32_t pnum, off_t at);

/**
 * \brief Clear a bit in the first \p copies volume table records found.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param copies                        How many to damage.
 *
 * \return How many were damaged.
 */
uint32_t corrupt_volume_tables(psa_key_id_t ikm_key_id, uint32_t copies);

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

#if defined(CONFIG_FLASH_SIMULATOR)

/**
 * \brief Fail every flash write once \p after bytes have gone through.
 */
void flash_fail_writes_after(uint32_t after);

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

#endif /* CONFIG_FLASH_SIMULATOR */

#endif /* COMMON_H */
