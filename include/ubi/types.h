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

#ifdef __cplusplus
extern "C" {
#endif

/* Defines ----------------------------------------------------------------- */

/** \defgroup ubi-values UBI values and limits
 * @{
 */

/** Longest volume name, in characters. */
#define UBI_VOLUME_NAME_MAX_LEN (16)

/** Most bytes \ref ubi_config.key_context may hold. */
#define UBI_KEY_CONTEXT_MAX_SIZE (32)

/** Reported in place of a volume identifier when none applies. */
#define UBI_VOL_ID_INVALID (UINT32_MAX)

/**@}*/

/* Types and type definitions ---------------------------------------------- */

/** \defgroup ubi-types-events UBI integrity events
 * @{
 */

/**
 * \brief Integrity and authenticity conditions reported by UBI.
 */
enum ubi_event_type {
	/** A header failed its CRC: an interrupted write or bit rot. */
	UBI_EVENT_HDR_CORRUPT,
	/** A header passed its CRC but not its CMAC: a field was changed and
	 *  the checksum recomputed, or it was sealed under another key. */
	UBI_EVENT_HDR_TAMPERED,
	/** A volume table copy could not be used. Damage and forgery look the
	 *  same, since repairing its checksum needs the key. */
	UBI_EVENT_VOLUME_TABLE_CORRUPT,
	/** One usable volume table copy is left, or the two disagree.
	 *  #UBI_MAINTENANCE_REPAIR clears it. */
	UBI_EVENT_VOLUME_TABLE_DEGRADED,
	/** A block claims a volume this device never created; it is queued for
	 *  reclaim. What a removed or shrunk volume left is taken back without
	 *  a report. */
	UBI_EVENT_LEB_ORPHANED,
	/** A block was retired after a failed write or erase. A failed erase
	 *  also leaves the device read-only until it is attached again. */
	UBI_EVENT_PEB_BAD,
	/** A sealed copy of a logical block no longer matches its checksum: a
	 *  change cut short, or damage. An older copy takes its place if there
	 *  is one; otherwise the block is kept as it reads. */
	UBI_EVENT_DATA_CORRUPT,
};

/**
 * \brief A single reported event.
 */
struct ubi_event {
	/** What was detected. */
	enum ubi_event_type type;
	/** Physical erase block concerned. */
	uint32_t pnum;
	/** Volume concerned, or #UBI_VOL_ID_INVALID when not established. */
	uint32_t vol_id;
	/** Logical erase block concerned, meaningful only with \p vol_id. */
	uint32_t lnum;
};

/**
 * \brief Event notification callback.
 *
 *        Runs with the device lock held and must not block; a call back into
 *        UBI returns \c -EDEADLK. Events only report: UBI has already acted.
 *
 * \param[in] event                     Detected condition.
 * \param[in] user_context              User context from \ref ubi_config.
 */
typedef void (*ubi_event_cb_t)(const struct ubi_event *event,
			       void *user_context);

/**@}*/

/** \defgroup ubi-types-info UBI introspection
 * @{
 */

/**
 * \brief Geometry, block accounting and the counters a rollback check
 *        compares.
 *
 *        Each counter says when it can go down in normal use. Kept where the
 *        flash cannot reach them, any other drop shows that the flash was
 *        taken back to an older copy of itself.
 */
struct ubi_device_info {
	/** Physical erase blocks in the partition. */
	uint32_t peb_count;
	/** Size of one physical erase block in bytes. */
	uint32_t peb_size;
	/** Bytes usable per LEB: \p peb_size minus the two 64-byte headers. */
	uint32_t leb_size;
	/** Write granularity of the flash. The offset and the length given to
	 *  \ref ubi_leb_write_at are multiples of it. */
	uint32_t write_block_size;

	/** Erased and stamped, allocatable without an erase. */
	uint32_t free_pebs;
	/** Waiting to be erased: released, unmapped, blank or left by an
	 *  earlier image. A write that finds nothing free erases one itself. */
	uint32_t reclaimable_pebs;
	/** Blocks #UBI_MAINTENANCE_RELOCATE would move now. */
	uint32_t relocatable_pebs;
	/** Retired after a failed write or erase. Held in RAM only: a repair
	 *  or the next attach gives them another chance. */
	uint32_t bad_pebs;
	/** Damaged behind a valid erase counter header and kept for what they
	 *  hold. #UBI_MAINTENANCE_DISCARD erases them. */
	uint32_t corrupt_pebs;

	/** Logical blocks \ref ubi_volume_create and \ref ubi_volume_resize can
	 *  still give out. A LEB costs a physical block once written. */
	uint32_t free_lebs;

	/** Volumes currently defined. */
	uint32_t volume_count;
	/** Drawn by every \ref ubi_device_format and constant until the
	 *  next. */
	uint32_t image_seq;

	/** Lowest erase count across the device. */
	uint32_t min_erase_count;
	/** Highest erase count across the device. */
	uint32_t max_erase_count;

	/** Volume table revision: 1 after a format, one more for every volume
	 *  create, resize and remove and every table repair. Never goes down
	 *  within one \p image_seq. */
	uint32_t revision;
	/** Highest sequence number on the flash at attach, raised by every
	 *  header written since. Goes down across a reboot only once the blocks
	 *  carrying the highest numbers are erased: by \ref ubi_leb_erase, by
	 *  reclaim after an unmap, a shrink, a removal or a change cut short,
	 *  or after a write that failed. */
	uint64_t max_sqnum;
	/** Blocks in service whose erase counter header verified for this
	 *  image. Goes down when a header is damaged, a write is cut short or a
	 *  block is retired, and up when reclaim stamps a block. */
	uint32_t healthy_pebs;
	/** Erase counts of the \p healthy_pebs, summed. Goes up with every
	 *  erase and down only when a block leaves \p healthy_pebs. */
	uint64_t total_erase_count;
};

/**
 * \brief Volume properties.
 */
struct ubi_volume_info {
	/** Identifier assigned at creation. */
	uint32_t vol_id;
	/** Logical erase blocks reserved for this volume. */
	uint32_t leb_count;
	/** How many of them currently have a physical block behind them. */
	uint32_t mapped_lebs;
	/** NUL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
};

/**
 * \brief State of a single logical erase block.
 */
struct ubi_leb_info {
	/** True when a physical block backs this LEB. */
	bool mapped;
	/** Erase count of the backing block, zero when unmapped. */
	uint32_t erase_count;
};

/**@}*/

/** \defgroup ubi-types-state UBI state check
 * @{
 */

/**
 * \brief Verdict returned by \ref ubi_state_cb_t.
 */
enum ubi_state_verdict {
	/** UBI may carry on. */
	UBI_STATE_TRUSTED = 0,
	/** UBI must stop, and stays stopped until the device is attached
	 *  again; \ref ubi_device_init returns \c -EROFS. */
	UBI_STATE_UNTRUSTED = 1,
};

/**
 * \brief Trust check, called at the end of every attach and again every
 *        \c CONFIG_UBI_STATE_CHECK_INTERVAL flash writes.
 *
 *        Rollback detection belongs here. It runs before the write it
 *        guards, so a refusal leaves nothing written: commit the new values
 *        to the trusted store before returning #UBI_STATE_TRUSTED.
 *        #UBI_STATE_UNTRUSTED holds until the next attach: every call that
 *        would write returns \c -EROFS, while reads keep working.
 *
 *        Runs with the device lock held and must not block; a call back into
 *        UBI returns \c -EDEADLK.
 *
 * \param[in] info                      State of the device as it stands.
 * \param[in] user_context              User context from \ref ubi_config.
 *
 * \return Verdict deciding whether UBI may carry on.
 */
typedef enum ubi_state_verdict (*ubi_state_cb_t)(
	const struct ubi_device_info *info, void *user_context);

/**@}*/

/** \defgroup ubi-types-config UBI configuration
 * @{
 */

/**
 * \brief Everything UBI needs to open a partition.
 *
 *        \ref ubi_device_format and \ref ubi_device_init take the same
 *        configuration; other keying material makes the device unreadable.
 */
struct ubi_config {
	/** Fixed partition to manage, from \c PARTITION_ID(). */
	uint8_t flash_area_id;

	/**
	 * Handle of the input keying material in the PSA key store.
	 *
	 * UBI derives its own keys from it with HKDF-SHA256 and never reads
	 * the material itself. The key carries \c PSA_KEY_USAGE_DERIVE, permits
	 * \c PSA_ALG_HKDF(PSA_ALG_SHA_256) and is unique per device. A second
	 * partition under the same key needs a \p key_context of its own: the
	 * MAC binds a header to its block number, not to its partition.
	 */
	psa_key_id_t ikm_key_id;

	/** Optional bytes appended to the key derivation info, the same at
	 *  every format and attach of the partition. \c NULL with a size of
	 *  zero appends nothing. */
	const uint8_t *key_context;

	/** Bytes at \p key_context, at most #UBI_KEY_CONTEXT_MAX_SIZE. */
	size_t key_context_size;

	/** Integrity event sink. Required. */
	ubi_event_cb_t event_cb;

	/** Trust check. Required. */
	ubi_state_cb_t state_cb;

	/** Passed back to both callbacks. */
	void *user_context;
};

/**
 * \brief Volume creation parameters.
 */
struct ubi_volume_config {
	/** NUL-terminated, 1 to #UBI_VOLUME_NAME_MAX_LEN characters, unique
	 *  within the device. */
	const char *name;
	/** Number of logical erase blocks to reserve. */
	uint32_t leb_count;
};

/**@}*/

/** \defgroup ubi-types-maintenance UBI maintenance
 * @{
 */

/**
 * \brief Deferred housekeeping, performed only when the application asks.
 */
enum ubi_maintenance_op {
	/** Erase released blocks and stamp them, refilling the free pool. */
	UBI_MAINTENANCE_RECLAIM,
	/** Move rarely rewritten data off the least worn blocks onto a more
	 *  worn free one. Needs a free block. */
	UBI_MAINTENANCE_RELOCATE,
	/** Bring the volume table copies back into agreement, as
	 *  #UBI_EVENT_VOLUME_TABLE_DEGRADED asks, and give retired blocks
	 *  another chance. */
	UBI_MAINTENANCE_REPAIR,
	/** Erase the blocks kept as corrupt and put them back in service.
	 *  Attach refuses a partition once they reach a twentieth of its good
	 *  blocks, rounded down, or eight when that is zero. */
	UBI_MAINTENANCE_DISCARD,
};

/**
 * \brief Outcome of one \ref ubi_maintenance call.
 */
struct ubi_maintenance_result {
	/** Operations carried out, never more than the requested budget. */
	uint32_t performed;
	/** Operations still outstanding; zero means there is nothing left. */
	uint32_t remaining;
};

/**@}*/

/** \defgroup ubi-types-device UBI device handle
 * @{
 */

/**
 * \brief UBI device handle (opaque).
 *
 *        Allocate \ref ubi_device_size bytes and drive it through the API.
 *        Calls on one handle are serialised, so threads may share it.
 */
struct ubi_device;

/**@}*/

#ifdef __cplusplus
}
#endif

#endif /* UBI_TYPES_H */
