/**
 * \file    types.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) types and limits.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_TYPES_H
#define UBI_TYPES_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PSA headers: */
#include <psa/crypto_types.h>

/* Defines ----------------------------------------------------------------- */

/** Longest volume name, in characters. */
#define UBI_VOLUME_NAME_MAX_LEN (16)

/** Most bytes \ref ubi_config.key_context may hold. */
#define UBI_KEY_CONTEXT_MAX_SIZE (32)

/** Volume identifier that stands for no volume. */
#define UBI_VOL_ID_INVALID (UINT32_MAX)

/* Types and type definitions ---------------------------------------------- */

/* Device handle */

/**
 * \brief UBI device handle (opaque).
 *
 *        Allocate \ref ubi_device_size bytes. Calls on one handle are
 *        serialised, so threads may share it.
 */
struct ubi_device;

/* Events */

/**
 * \brief Integrity events. What to do on each: docs/operations.md.
 */
enum ubi_event_type {
	/** A header failed its CRC: a write or erase cut short, or bit rot. */
	UBI_EVENT_HDR_CORRUPT,
	/** A header failed authentication: modified, or written under another
	 *  key. */
	UBI_EVENT_HDR_TAMPERED,
	/** A volume table copy could not be used. */
	UBI_EVENT_VOLUME_TABLE_CORRUPT,
	/** Only one volume table copy is usable, or the copies differ; run
	 *  #UBI_MAINTENANCE_REPAIR. */
	UBI_EVENT_VOLUME_TABLE_DEGRADED,
	/** A block names a volume this device never created; it is queued for
	 *  reclaim. */
	UBI_EVENT_LEB_ORPHANED,
	/** A block was retired after a failed write or erase. A failed erase
	 *  also makes the device read-only. */
	UBI_EVENT_PEB_BAD,
	/** The data of a LEB failed its checksum: a change cut short, or
	 *  damage. The previous contents are used if they exist; otherwise the
	 *  LEB keeps the data as it reads. */
	UBI_EVENT_DATA_CORRUPT,
};

/**
 * \brief One reported event.
 */
struct ubi_event {
	/** What was detected. */
	enum ubi_event_type type;
	/** Physical erase block concerned. */
	uint32_t pnum;
	/** Volume concerned, or #UBI_VOL_ID_INVALID when unknown. */
	uint32_t vol_id;
	/** LEB concerned; valid only with \p vol_id. */
	uint32_t lnum;
};

/* Information */

/**
 * \brief Device information, also passed to the state callback.
 *
 *        The last five fields are rollback counters; docs/security.md says
 *        when each of them may go down.
 */
struct ubi_device_info {
	/** Physical erase blocks in the partition. */
	uint32_t peb_count;
	/** Size of a physical erase block, in bytes. */
	uint32_t peb_size;
	/** Size of a LEB, in bytes. */
	uint32_t leb_size;
	/** Write granularity of the flash, in bytes. */
	uint32_t write_block_size;

	/** Volumes on the device. */
	uint32_t volume_count;
	/** LEBs not given to any volume. */
	uint32_t free_lebs;

	/** Physical erase blocks erased and ready for writing. */
	uint32_t free_pebs;
	/** Physical erase blocks waiting for #UBI_MAINTENANCE_RECLAIM. */
	uint32_t reclaimable_pebs;
	/** Physical erase blocks #UBI_MAINTENANCE_RELOCATE would move. */
	uint32_t relocatable_pebs;
	/** Damaged physical erase blocks kept until #UBI_MAINTENANCE_DISCARD.
	 *  Too many of them make attach fail. */
	uint32_t corrupt_pebs;
	/** Physical erase blocks retired after a failed write or erase. */
	uint32_t bad_pebs;

	/** Lowest erase count. */
	uint32_t min_erase_count;
	/** Highest erase count. */
	uint32_t max_erase_count;

	/** Random number drawn by every format. */
	uint32_t image_seq;
	/** Volume table revision, one more on every volume table update. */
	uint32_t revision;
	/** Highest header sequence number. */
	uint64_t max_sqnum;
	/** Physical erase blocks with a valid erase counter header. */
	uint32_t healthy_pebs;
	/** Sum of the erase counts of the \p healthy_pebs. */
	uint64_t total_erase_count;
};

/**
 * \brief Volume information.
 */
struct ubi_volume_info {
	/** Identifier assigned at creation. */
	uint32_t vol_id;
	/** NUL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
	/** Size in LEBs. */
	uint32_t leb_count;
	/** LEBs that are mapped. */
	uint32_t mapped_lebs;
};

/**
 * \brief LEB information.
 */
struct ubi_leb_info {
	/** The LEB is mapped. */
	bool mapped;
	/** Erase count of its physical erase block; zero when unmapped. */
	uint32_t erase_count;
};

/* Callbacks */

/**
 * \brief Verdict returned by \ref ubi_config.state_cb.
 */
enum ubi_state_verdict {
	/** The device may be used. */
	UBI_STATE_TRUSTED = 0,
	/** The device is read-only until the next attach, and an attach in
	 *  progress fails with \c -EROFS. */
	UBI_STATE_UNTRUSTED = 1,
};

/* Configuration */

/**
 * \brief Device configuration.
 *
 *        Format and attach take the same configuration: a different key or
 *        key context makes the device unreadable.
 */
struct ubi_config {
	/** Fixed partition to manage, from \c PARTITION_ID(). */
	uint8_t flash_area_id;

	/**
	 * PSA key from which UBI derives its own keys with HKDF-SHA256.
	 *
	 * The key permits \c PSA_KEY_USAGE_DERIVE with
	 * \c PSA_ALG_HKDF(PSA_ALG_SHA_256) and is unique per device.
	 * Partitions that share it need different \p key_context values.
	 */
	psa_key_id_t ikm_key_id;

	/** Optional bytes mixed into the key derivation, e.g. the partition
	 *  name. \c NULL and a size of zero for none. */
	const uint8_t *key_context;

	/** Bytes at \p key_context, at most #UBI_KEY_CONTEXT_MAX_SIZE. */
	size_t key_context_size;

	/**
	 * Event callback. Required.
	 *
	 * Called with the device locked: it must not block, and a call back
	 * into UBI returns \c -EDEADLK. UBI has already handled the event.
	 */
	void (*event_cb)(const struct ubi_event *event, void *user_context);

	/**
	 * State callback, deciding whether to trust the device. Required.
	 *
	 * Called at the end of every attach, and before a write once
	 * \c CONFIG_UBI_STATE_CHECK_INTERVAL flash writes have passed. Compare
	 * the rollback counters in \p info with values kept in a trusted
	 * store, and store the new values before returning #UBI_STATE_TRUSTED;
	 * see docs/security.md. After #UBI_STATE_UNTRUSTED every write returns
	 * \c -EROFS until the next attach, while reads still work.
	 *
	 * Called with the device locked: it must not block, and a call back
	 * into UBI returns \c -EDEADLK.
	 */
	enum ubi_state_verdict (*state_cb)(const struct ubi_device_info *info,
					   void *user_context);

	/** Passed back to both callbacks. */
	void *user_context;
};

/**
 * \brief Volume creation parameters.
 */
struct ubi_volume_config {
	/** NUL-terminated name of 1 to #UBI_VOLUME_NAME_MAX_LEN characters,
	 *  unique within the device. */
	const char *name;
	/** Size in LEBs. */
	uint32_t leb_count;
};

/* Maintenance */

/**
 * \brief Maintenance operations, run by \ref ubi_maintenance.
 */
enum ubi_maintenance_op {
	/** Erase released blocks, refilling the free pool. */
	UBI_MAINTENANCE_RECLAIM,
	/** Move rarely changed data onto more worn blocks: wear levelling. */
	UBI_MAINTENANCE_RELOCATE,
	/** Restore the volume table copies and retry retired blocks. */
	UBI_MAINTENANCE_REPAIR,
	/** Erase the blocks kept as corrupt and return them to service. */
	UBI_MAINTENANCE_DISCARD,
};

/**
 * \brief Outcome of one \ref ubi_maintenance call.
 */
struct ubi_maintenance_result {
	/** Steps done, at most the budget. */
	uint32_t performed;
	/** Steps left. */
	uint32_t remaining;
};

#endif /* UBI_TYPES_H */
