/**
 * \file    ubi_private.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) library-internal types.
 *
 *          Private to the library. Consumers include \c <ubi/ubi.h>, where
 *          \ref ubi_device is an opaque forward declaration.
 *
 *          The handle is split by concern rather than kept as one flat
 *          record: the geometry, the keys, the volumes and the block
 *          bookkeeping each stand on their own.
 *
 *          Everything whose size follows the partition lives behind a
 *          pointer and is taken from the heap when the device attaches. How
 *          many blocks a partition holds is a property of the flash, not of
 *          the build, so it is not a build-time constant.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_PRIVATE_H
#define UBI_PRIVATE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/toolchain.h>

/* PSA headers: */
#include <psa/crypto_types.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_volume_table.h"

/* Defines ----------------------------------------------------------------- */

/**
 * "UBI$" in ASCII, following the on-flash magics "UBI#" and "UBI!".
 *
 * Never written to the flash; it only marks an attached handle in RAM, so
 * that operating on uninitialised storage is caught rather than obeyed.
 */
#define UBI_DEVICE_MAGIC (0x55424924UL)

/** Entry in \ref ubi_volume.eba that has no physical block behind it. */
#define UBI_LEB_UNMAPPED (UINT16_MAX)

/** Fewer blocks than this leaves no room to work in. */
#define UBI_MIN_PEB_COUNT (4)

/** Block numbers are kept as 16-bit values, and one is spent on the
 *  unmapped sentinel. */
#define UBI_MAX_PEB_COUNT (UINT16_MAX - 1)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief Lifecycle state of a physical erase block.
 */
enum ubi_peb_state {
	/** Contents unknown: never used, or left by an earlier image. Must be
	 *  erased and stamped before it can serve. */
	UBI_PEB_UNKNOWN = 0,
	/** Erased and carrying a valid EC header; allocatable with no erase. */
	UBI_PEB_FREE,
	/** Carries a valid VID header and backs a logical erase block. */
	UBI_PEB_MAPPED,
	/** Released by its logical block, awaiting #UBI_MAINTENANCE_RECLAIM. */
	UBI_PEB_RECLAIM,
	/** Retired after tampering or a persistent I/O error. Held in RAM
	 *  only, never written to the flash, so a wrong key does not condemn
	 *  a block permanently. */
	UBI_PEB_BAD,
};

BUILD_ASSERT(UBI_PEB_BAD <= UINT8_MAX,
	     "block states must fit the byte they are stored in");

/**
 * \brief Fixed dimensions of the managed partition.
 */
struct ubi_geometry {
	/** Physical erase blocks in the partition. */
	uint32_t peb_count;
	/** Size of one physical erase block in bytes. */
	uint32_t peb_size;
	/** Bytes usable per logical erase block. */
	uint32_t leb_size;
	/** Write granularity of the underlying flash. */
	uint32_t write_block_size;
	/** Byte an erase leaves behind. Reported by the driver rather than
	 *  assumed to be 0xFF, and used both to recognise a blank block and to
	 *  pad a write up to the write granularity. */
	uint8_t erase_value;
};

/**
 * \brief Keys UBI derived for itself from the application's handle.
 */
struct ubi_keys {
	/** Authenticates EC and VID headers. */
	psa_key_id_t header;
	/** Authenticates the volume table record. */
	psa_key_id_t volume_table;
};

/**
 * \brief Hooks back into the application.
 */
struct ubi_callbacks {
	/** Integrity event sink. */
	ubi_event_cb_t event;
	/** Trust check. */
	ubi_state_cb_t state;
	/** Passed back to both. */
	void *user_context;
};

/**
 * \brief A volume as tracked at run time.
 */
struct ubi_volume {
	/** Identifier assigned at creation, never reused. */
	uint32_t vol_id;
	/** Logical erase blocks reserved for this volume. */
	uint32_t leb_count;
	/** NULL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
	/**
	 * Erase block association table: \p leb_count entries indexed by
	 * logical block number, each holding a physical block number or
	 * #UBI_LEB_UNMAPPED.
	 *
	 * Points into \ref ubi_volumes.eba_pool. Linux UBI indexes a flat
	 * array by logical block number too, and it makes lookup O(1) with no
	 * allocation.
	 */
	uint16_t *eba;
};

/**
 * \brief Every volume the application declared, plus the storage they share.
 */
struct ubi_volumes {
	/** Volumes currently defined, occupying the first \ref entries. */
	uint32_t count;
	/** Highest volume identifier ever handed out. Monotonic, so an
	 *  identifier is never reused after its volume is removed. */
	uint32_t id_watermark;
	/** Revision of the on-flash record, incremented on every change. */
	uint32_t revision;
	/** Volumes as reconstructed at attach. */
	struct ubi_volume entries[CONFIG_UBI_MAX_NR_OF_VOLUMES];
	/** Storage carved up between their \c eba arrays, one entry per
	 *  physical erase block. Nothing larger can be needed: a mapped
	 *  logical block occupies a physical one. */
	uint16_t *eba_pool;
	/** Entries of \ref eba_pool already handed out. */
	uint32_t eba_used;
};

/**
 * \brief The volume that holds the volume table record, and what attach
 *        learned about its copies.
 *
 *        An ordinary volume in every respect the mapping cares about, which
 *        is why it is one, but it stands apart from \ref ubi_volumes: it has
 *        to be reachable before the record that declares those entries has
 *        been read, it must not be visible to the application, and its
 *        mapping cannot come from a pool that formatting never allocates.
 */
struct ubi_volume_table {
	/** The volume itself, for the ordinary lookup path. */
	struct ubi_volume volume;
	/** Physical block holding each copy, indexed by copy number. */
	uint16_t eba[UBI_VOLUME_TABLE_LEB_COUNT];
	/** Sequence number each copy carries. */
	uint64_t sqnum[UBI_VOLUME_TABLE_LEB_COUNT];
	/** Which copy is in force. The other one is the copy an update
	 *  overwrites first, so that a complete copy always survives. */
	uint32_t current;
};

/**
 * \brief Per-block bookkeeping, rebuilt from the flash on every attach.
 *
 *        Both arrays are indexed by physical block number, so their size
 *        follows the erase block count rather than the size of the flash.
 */
struct ubi_blocks {
	/** Lifecycle state of every physical erase block, holding values of
	 *  \ref ubi_peb_state narrowed to a byte. */
	uint8_t *state;
	/** Erase count of every physical erase block, from its EC header.
	 *  The flash field is 64-bit, but no flash survives more than a few
	 *  million erases, so 32 bits are kept in RAM. */
	uint32_t *erase_count;
};

/**
 * \brief An attached UBI device.
 *
 *        Declared opaque in the public header; this is its real layout.
 */
struct ubi_device {
	/** #UBI_DEVICE_MAGIC once attached. Guards against operating on
	 *  uninitialised storage and against attaching the same handle
	 *  twice. */
	uint32_t magic;

	/** Managed partition, open for the lifetime of the attachment. */
	const struct flash_area *flash_area;

	/** Identifies this image; blocks carrying any other value are
	 *  foreign and treated as #UBI_PEB_UNKNOWN. */
	uint32_t image_seq;

	/** Highest sequence number seen or issued so far. Reported as
	 *  \ref ubi_device_info.global_sqnum. */
	uint64_t global_sqnum;

	/** Metadata writes since the state callback was last consulted. */
	uint32_t writes_since_check;

	/** Set once the state callback has withdrawn its trust. Latched: only
	 *  a fresh attach clears it. */
	bool untrusted;

	/** Serialises every public operation on this device. */
	struct k_mutex lock;

	/** Dimensions of the partition. */
	struct ubi_geometry geometry;

	/** Keys derived from the application's handle. */
	struct ubi_keys keys;

	/** Hooks back into the application. */
	struct ubi_callbacks callbacks;

	/** Volumes and their logical-to-physical mappings. */
	struct ubi_volumes volumes;

	/** The volume that describes those volumes. */
	struct ubi_volume_table volume_table;

	/** State and wear of every physical erase block. */
	struct ubi_blocks blocks;
};

#endif /* UBI_PRIVATE_H */
