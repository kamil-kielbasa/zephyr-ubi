/**
 * \file    ubi_io.h
 * \author  Kamil Kielbasa
 * \brief   Physical access to the managed partition.
 *
 *          The one place where a physical erase block number becomes a flash
 *          offset, and therefore the one place where that number has to be
 *          checked. Everything above works in block numbers and offsets
 *          within a block, and nothing above calls the flash API directly.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_IO_H
#define UBI_IO_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* UBI headers: */
#include "ubi_header.h"

/* Types and type definitions ---------------------------------------------- */

struct ubi_device;

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Read bytes from within one physical erase block.
 *
 * \param[in] ubi                       Device holding the partition.
 * \param pnum                          Physical erase block to read.
 * \param offset                        Byte offset within that block.
 * \param[out] buffer                   Destination.
 * \param length                        Bytes to read.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p pnum is outside the partition, or the range leaves the block.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_io_read(const struct ubi_device *ubi, uint32_t pnum,
		     uint32_t offset, uint8_t *buffer, size_t length);

/**
 * \brief Write bytes into one physical erase block.
 *
 * \param[in,out] ubi                   Device holding the partition; its
 *                                      count of writes since the last state
 *                                      check goes up by one.
 * \param pnum                          Physical erase block to write.
 * \param offset                        Byte offset within that block.
 * \param[in] buffer                    Bytes to write.
 * \param length                        Bytes to write.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p pnum is outside the partition, or the range leaves the block.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_io_write(struct ubi_device *ubi, uint32_t pnum, uint32_t offset,
		      const uint8_t *buffer, size_t length);

/**
 * \brief Erase one physical erase block.
 *
 * \param[in] ubi                       Device holding the partition.
 * \param pnum                          Physical erase block to erase.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p pnum is outside the partition.
 * \retval -EIO
 *         The flash driver failed.
 */
int ubi_impl_io_erase(const struct ubi_device *ubi, uint32_t pnum);

/**
 * \brief Read from the data area, counting the offset from where the data
 *        begins rather than from the start of the block.
 *
 *        Same arguments and same returns as \ref ubi_impl_io_read otherwise.
 *        Linux UBI keeps the same pair for the same reason: the two headers
 *        in front of the data are nobody else's business.
 */
static inline int ubi_impl_io_read_data(const struct ubi_device *ubi,
					uint32_t pnum, uint32_t offset,
					uint8_t *buffer, size_t length)
{
	return ubi_impl_io_read(ubi, pnum, UBI_DATA_OFFSET + offset, buffer,
				length);
}

/**
 * \brief Write to the data area, counting the offset the same way.
 */
static inline int ubi_impl_io_write_data(struct ubi_device *ubi, uint32_t pnum,
					 uint32_t offset, const uint8_t *buffer,
					 size_t length)
{
	return ubi_impl_io_write(ubi, pnum, UBI_DATA_OFFSET + offset, buffer,
				 length);
}

#endif /* UBI_IO_H */
