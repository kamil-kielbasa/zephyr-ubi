/**
 * \file    ubi_private.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) library-internal types.
 *
 *          Private to the library. Consumers include \c <ubi/ubi.h>, where
 *          \ref ubi_device is an opaque forward declaration.
 *
 *          The device handle is split by concern rather than kept as one flat
 *          record: geometry, keys, callbacks, the volume table and the block
 *          bookkeeping each stand on their own, so a function can take just
 *          the part it needs.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_PRIVATE_H
#define UBI_PRIVATE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

/* PSA headers: */
#include <psa/crypto_types.h>

/* UBI headers: */
#include <ubi/ubi.h>

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

/**
 * Volume identifier reserved for the volume table itself.
 *
 * Linux UBI calls this the layout volume; the name here says what it holds.
 */
#define UBI_VOLUME_TABLE_VOL_ID (0xFFFFFFFEUL)

/** Logical blocks holding the volume table record, written alternately. */
#define UBI_VOLUME_TABLE_LEB_COUNT (2)

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
	/** Integrity event sink, or \c NULL. */
	ubi_event_cb_t event;
	/** Rollback check, or \c NULL. */
	ubi_freshness_cb_t freshness;
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
	 * Points into \ref ubi_volume_table.eba_pool. Linux UBI indexes a flat
	 * array by logical block number too, and it makes lookup O(1) with no
	 * allocation.
	 */
	uint16_t *eba;
};

/**
 * \brief Every volume on the device, plus the storage they share.
 */
struct ubi_volume_table {
	/** Volumes currently defined, occupying the first \ref entries. */
	uint32_t count;
	/** Highest volume identifier ever handed out. Monotonic, so an
	 *  identifier is never reused after its volume is removed. */
	uint32_t id_watermark;
	/** Volumes as reconstructed at attach. */
	struct ubi_volume entries[CONFIG_UBI_MAX_NR_OF_VOLUMES];
	/** Backing storage carved up between the volumes' \c eba arrays. */
	uint16_t eba_pool[CONFIG_UBI_MAX_NR_OF_LEBS];
	/** Entries of \ref eba_pool already handed out. */
	uint32_t eba_used;
	/** Physical blocks holding the volume table record, written
	 *  alternately so an interrupted update leaves the previous record
	 *  intact. */
	uint16_t peb[UBI_VOLUME_TABLE_LEB_COUNT];
};

/**
 * \brief Per-block bookkeeping, rebuilt from the flash on every attach.
 */
struct ubi_block_table {
	/** Lifecycle state of every physical erase block, holding values of
	 *  \ref ubi_peb_state. */
	uint8_t state[CONFIG_UBI_MAX_NR_OF_PEBS];
	/** Erase count of every physical erase block, from its EC header. */
	uint32_t erase_count[CONFIG_UBI_MAX_NR_OF_PEBS];
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

	/** Managed partition, opened for the lifetime of the attachment. */
	const struct flash_area *flash_area;

	/** Identifies this image; blocks carrying any other value are
	 *  foreign and treated as #UBI_PEB_UNKNOWN. */
	uint32_t image_seq;

	/** Serialises every public operation on this device. */
	struct k_mutex lock;

	/** Dimensions of the partition. */
	struct ubi_geometry geometry;

	/** Counters the application anchors to detect a rollback. Handed to
	 *  the freshness callback as-is. */
	struct ubi_freshness freshness;

	/** Keys derived from the application's handle. */
	struct ubi_keys keys;

	/** Hooks back into the application. */
	struct ubi_callbacks callbacks;

	/** Volumes and their logical-to-physical mappings. */
	struct ubi_volume_table volumes;

	/** State and wear of every physical erase block. */
	struct ubi_block_table blocks;
};

#endif /* UBI_PRIVATE_H */
