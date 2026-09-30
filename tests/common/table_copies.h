/**
 * \file    table_copies.h
 * \author  Kamil Kielbasa
 * \brief   The volume table copies on the flash, found, read, damaged and
 *          forged behind the library's back.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef TABLE_COPIES_H
#define TABLE_COPIES_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_volume_table.h"

/* Function declarations --------------------------------------------------- */

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
 * \brief Find the blocks carrying a volume table copy, of whatever image, in
 *        the order they sit in the partition.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param[out] pnums                    Receives their block numbers.
 * \param capacity                      Room in \p pnums.
 *
 * \return How many there are, which may be more than \p capacity.
 */
uint32_t volume_table_blocks(psa_key_id_t ikm_key_id, uint32_t *pnums,
			     uint32_t capacity);

/**
 * \brief Find the block each volume table copy is in, by copy number,
 *        failing unless each is in exactly one.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param[out] pnums                    Receives the block of each copy.
 */
void volume_table_copy_blocks(psa_key_id_t ikm_key_id,
			      uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT]);

/**
 * \brief Read the record a volume table copy holds, as far as its header
 *        says it goes.
 *
 * \return Bytes copied into \p record.
 */
size_t volume_table_record_read(psa_key_id_t ikm_key_id, uint32_t pnum,
				uint8_t *record, size_t capacity);

/**
 * \brief Seal a volume table copy afresh over \p data_size bytes of its data
 *        area, as a relocation would, leaving the bytes themselves alone.
 */
void volume_table_reseal(psa_key_id_t ikm_key_id, uint32_t pnum,
			 uint32_t data_size);

/**
 * \brief Put \p record in a volume table copy, authenticated and sealed as
 *        the library would have done it.
 *
 * \param[in,out] record                Record bytes; the MAC is computed
 *                                      into the last #UBI_MAC_SIZE of them.
 * \param length                        Bytes of record, MAC included.
 */
void volume_table_record_forge(psa_key_id_t ikm_key_id, uint32_t pnum,
			       uint8_t *record, size_t length);

/**
 * \brief Find the volume table copies an attach could adopt: both headers
 *        verify, the data matches its seal and the record verifies.
 *
 * \param ikm_key_id                    Keying material the device attaches
 *                                      with.
 * \param[out] pnums                    Receives their block numbers, in the
 *                                      order they sit in the partition, if
 *                                      not \c NULL.
 * \param[out] revisions                Receives their revisions, in the same
 *                                      order, if not \c NULL.
 * \param capacity                      Room in each.
 *
 * \return How many there are, which may be more than \p capacity.
 */
uint32_t volume_table_adoptable(psa_key_id_t ikm_key_id, uint32_t *pnums,
				uint32_t *revisions, uint32_t capacity);

#endif /* TABLE_COPIES_H */
