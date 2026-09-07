/**
 * \file    ubi_priv.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) library-internal types.
 *
 *          Private to the library. Consumers include \c <ubi/ubi.h>, where
 *          \ref ubi_device is an opaque forward declaration.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_PRIV_H
#define UBI_PRIV_H

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

/** Marks an attached device; any other value means uninitialised storage. */
#define UBI_DEVICE_MAGIC (0x55424924)

/** Entry in \ref ubi_volume.eba that has no physical block behind it. */
#define UBI_LEB_UNMAPPED (UINT16_MAX)

/** Volume identifier reserved for the internal volume table. */
#define UBI_LAYOUT_VOL_ID (0xFFFFFFFEUL)

/** Logical erase blocks holding the volume table, written alternately. */
#define UBI_LAYOUT_LEB_COUNT (2)

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
 * \brief A volume as tracked at run time.
 */
struct ubi_volume {
	/** Identifier assigned at creation, never reused. */
	uint32_t vol_id;
	/** Logical erase blocks reserved for this volume. */
	uint32_t leb_count;
	/** NUL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
	/**
	 * Erase block association table: \p leb_count entries indexed by
	 * logical block number, each holding a physical block number or
	 * #UBI_LEB_UNMAPPED.
	 *
	 * Points into \ref ubi_device.eba_pool. A flat array indexed by
	 * \c lnum is what Linux UBI uses too, and it makes lookup O(1) with no
	 * allocation.
	 */
	uint16_t *eba;
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
	/** Physical erase blocks in the partition. */
	uint32_t peb_count;
	/** Size of one physical erase block in bytes. */
	uint32_t peb_size;
	/** Bytes usable per logical erase block. */
	uint32_t leb_size;
	/** Write granularity of the underlying flash. */
	uint32_t write_block_size;

	/** Identifies this image; blocks carrying any other value are
	 *  foreign and treated as #UBI_PEB_UNKNOWN. */
	uint32_t image_seq;
	/** Volume table revision, part of \ref ubi_freshness. */
	uint32_t revision;
	/** Highest sequence number issued so far; the next VID header write
	 *  takes this value and increments it. */
	uint64_t global_sqnum;

	/** Derived key authenticating EC and VID headers. */
	psa_key_id_t key_hdr;
	/** Derived key authenticating the volume table record. */
	psa_key_id_t key_layout;

	/** Integrity event sink, or \c NULL. */
	ubi_event_cb_t event_cb;
	/** Rollback check, or \c NULL. */
	ubi_freshness_cb_t freshness_cb;
	/** Passed back to both callbacks. */
	void *user_context;

	/** Serialises every public operation on this device. */
	struct k_mutex lock;

	/** Volumes currently defined, occupying the first entries of
	 *  \ref volumes. */
	uint32_t volume_count;
	/** Highest volume identifier ever handed out. Monotonic, so an
	 *  identifier is never reused after its volume is removed. */
	uint32_t vol_id_watermark;
	/** Volume table as reconstructed at attach. */
	struct ubi_volume volumes[CONFIG_UBI_MAX_NR_OF_VOLUMES];

	/** Backing storage carved up between the volumes' \c eba arrays. */
	uint16_t eba_pool[CONFIG_UBI_MAX_NR_OF_LEBS];
	/** Entries of \ref eba_pool already handed out. */
	uint32_t eba_used;

	/** Physical blocks holding the volume table, written alternately so
	 *  an interrupted update leaves the previous table intact. */
	uint16_t layout_peb[UBI_LAYOUT_LEB_COUNT];

	/** Lifecycle state of every physical erase block. */
	uint8_t peb_state[CONFIG_UBI_MAX_NR_OF_PEBS];
	/** Erase count of every physical erase block, from its EC header. */
	uint32_t peb_erase_count[CONFIG_UBI_MAX_NR_OF_PEBS];
};

/* Module interface variables and constants -------------------------------- */
/* Extern variables and constant declarations ------------------------------ */
/* Module interface function declarations ---------------------------------- */

#endif /* UBI_PRIV_H */
