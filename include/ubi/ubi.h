/**
 * \file    ubi.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) public API.
 *
 *          UBI maps logical erase blocks (LEB) onto physical erase blocks
 *          (PEB), spreads wear across the partition, survives power loss and
 *          authenticates its own metadata with AES-CMAC.
 *
 *          UBI never encrypts or authenticates application data. Only the EC
 *          and VID headers and the internal volume table are protected;
 *          anything written through \ref ubi_leb_change or
 *          \ref ubi_leb_write_at is stored verbatim. If it has to be
 *          confidential or tamper-evident, the layer above seals it.
 *
 *          All key material enters as a PSA key handle and never crosses this
 *          interface as raw bytes.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_H
#define UBI_H

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

/** Maximum length of a volume name, excluding the terminating NUL. */
#define UBI_VOLUME_NAME_MAX_LEN (15)

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
	/** Header CRC failed: an interrupted write or bit rot, not necessarily
	 *  an attack. */
	UBI_EVENT_HDR_CORRUPT,
	/** Header CRC passed but the CMAC did not: a field was changed and the
	 *  checksum recomputed. */
	UBI_EVENT_HDR_TAMPERED,
	/** The volume table record failed its CMAC. */
	UBI_EVENT_LAYOUT_TAMPERED,
	/** A physical erase block was retired after a persistent I/O error. */
	UBI_EVENT_PEB_BAD,
	/** The freshness callback rejected the on-flash state. */
	UBI_EVENT_FRESHNESS_FAIL,
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
 *        Called synchronously from whichever context detected the condition,
 *        with the device lock held. Do not call back into UBI and do not
 *        block.
 *
 *        Events are informational and never change what UBI does next. The
 *        one exception is #UBI_EVENT_FRESHNESS_FAIL, which accompanies a
 *        failing attach.
 *
 * \param[in] event                     Detected condition.
 * \param[in] user_context              User context from \ref ubi_config.
 */
typedef void (*ubi_event_cb_t)(const struct ubi_event *event,
			       void *user_context);

/**@}*/

/** \defgroup ubi-types-freshness UBI rollback detection
 * @{
 */

/**
 * \brief On-flash freshness counters, presented to the application on attach.
 *
 *        Keep these in a trusted, rollback-protected location (PSA ITS, an
 *        RPMC counter, a secure element) and compare on the next boot.
 *        Neither counter ever decreases during normal operation.
 *
 *        This detects a rollback of the whole device. It does not detect a
 *        rollback of a single LEB: restoring one PEB to an older authentic
 *        image leaves the global maximum untouched. Closing that gap needs
 *        per-LEB state in the trusted store, updated on every write. Linux
 *        UBIFS declares the same limitation.
 */
struct ubi_freshness {
	/** Incremented whenever the volume table changes. */
	uint32_t revision;
	/** Highest sequence number issued so far; incremented on every VID
	 *  header write. */
	uint64_t global_sqnum;
};

/**
 * \brief Verdict returned by \ref ubi_freshness_cb_t.
 */
enum ubi_freshness_verdict {
	/** Proceed with the attach. */
	UBI_FRESHNESS_ACCEPT = 0,
	/** Abort the attach; \ref ubi_device_init returns \c -EROFS. */
	UBI_FRESHNESS_REJECT = 1,
};

/**
 * \brief Freshness check callback, invoked once near the end of an otherwise
 *        successful attach.
 *
 *        Compare \p freshness against the values last committed. When
 *        returning #UBI_FRESHNESS_ACCEPT, commit the new values first: the
 *        attach completes immediately afterwards.
 *
 *        Leaving this callback \c NULL disables rollback detection.
 *
 * \param[in] freshness                 Counters read from the flash.
 * \param[in] user_context              User context from \ref ubi_config.
 *
 * \return Verdict deciding whether the attach continues.
 */
typedef enum ubi_freshness_verdict (*ubi_freshness_cb_t)(
	const struct ubi_freshness *freshness, void *user_context);

/**@}*/

/** \defgroup ubi-types-config UBI configuration
 * @{
 */

/**
 * \brief Everything UBI needs to open a partition.
 *
 *        The same configuration must be given to \ref ubi_device_format and
 *        \ref ubi_device_init. A different \p ikm_key_id makes the device
 *        unreadable.
 */
struct ubi_config {
	/** Fixed partition to manage, from \c FIXED_PARTITION_ID(). */
	uint8_t flash_area_id;

	/**
	 * Handle of the input keying material, held in the PSA key store.
	 *
	 * UBI derives its own keys from it with HKDF-SHA256 and never reads,
	 * copies or stores the material itself. The key must carry
	 * \c PSA_KEY_USAGE_DERIVE and permit \c PSA_ALG_HKDF(PSA_ALG_SHA_256).
	 *
	 * The key must be unique per device. The derivation is salted with a
	 * 32-bit image sequence number, so a fleet sharing one handle will
	 * eventually see two devices derive the same key, and a PEB moved
	 * between them would verify.
	 */
	psa_key_id_t ikm_key_id;

	/** Integrity event sink, or \c NULL. */
	ubi_event_cb_t event_cb;

	/** Rollback check, or \c NULL to disable it. */
	ubi_freshness_cb_t freshness_cb;

	/** Passed back to both callbacks. */
	void *user_context;
};

/**
 * \brief Volume creation parameters.
 */
struct ubi_volume_config {
	/** NUL-terminated, at most #UBI_VOLUME_NAME_MAX_LEN characters,
	 *  unique within the device. */
	const char *name;
	/** Number of logical erase blocks to reserve. */
	uint32_t leb_count;
};

/**@}*/

/** \defgroup ubi-types-info UBI introspection
 * @{
 */

/**
 * \brief Device geometry and current block accounting.
 */
struct ubi_device_info {
	/** Physical erase blocks in the partition. */
	uint32_t peb_count;
	/** Size of one physical erase block in bytes. */
	uint32_t peb_size;
	/** Bytes usable per LEB: \p peb_size minus the two 64-byte headers. */
	uint32_t leb_size;
	/** Write granularity of the underlying flash. Both \p offset and
	 *  \p len given to \ref ubi_leb_write_at must be multiples of it. */
	uint32_t write_block_size;

	/** Erased and stamped, allocatable without an erase. */
	uint32_t free_pebs;
	/** Released, awaiting #UBI_MAINTENANCE_RECLAIM before reuse. */
	uint32_t reclaimable_pebs;
	/** Retired after tampering or a persistent I/O error, never reused. */
	uint32_t bad_pebs;

	/** Volumes currently defined. */
	uint32_t volume_count;
	/** Identifies this UBI image; regenerated by every
	 *  \ref ubi_device_format. */
	uint32_t image_seq;

	/** Current freshness counters, the same values the callback saw. */
	struct ubi_freshness freshness;

	/** Lowest erase count across the device. */
	uint32_t min_erase_count;
	/** Highest erase count across the device. A wide spread against
	 *  \p min_erase_count means #UBI_MAINTENANCE_RELOCATE is due. */
	uint32_t max_erase_count;
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

/** \defgroup ubi-types-maintenance UBI maintenance
 * @{
 */

/**
 * \brief Deferred housekeeping, performed only when the application asks.
 */
enum ubi_maintenance_op {
	/** Erase released blocks and stamp them, refilling the free pool. */
	UBI_MAINTENANCE_RECLAIM,
	/** Move rarely rewritten data off the least worn blocks to even out
	 *  erase counts. */
	UBI_MAINTENANCE_RELOCATE,
	/** Rewrite blocks whose reads look marginal, refreshing the charge. */
	UBI_MAINTENANCE_SCRUB,
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
 *        Allocate storage of \ref ubi_device_size bytes and drive it through
 *        the public API; the layout is library-internal.
 */
struct ubi_device;

/**@}*/

/* Module interface variables and constants -------------------------------- */
/* Extern variables and constant declarations ------------------------------ */
/* Module interface function declarations ---------------------------------- */

/** \defgroup ubi-api-device UBI device lifecycle
 * @{
 */

/**
 * \brief Size in bytes of a UBI device handle for this build.
 *
 *        The size depends on the build-time configuration, so it is a
 *        run-time value. Allocate at least this many bytes for the
 *        \ref ubi_device passed to \ref ubi_device_init.
 *
 * \return Size in bytes of \ref ubi_device.
 */
size_t ubi_device_size(void);

/**
 * \brief Turn a partition into an empty UBI device.
 *
 *        Destroys any existing content. Writes a fresh image sequence number
 *        and one volume table; it does not erase the whole partition. Blocks
 *        left over from earlier use carry a stale image sequence number, so
 *        UBI treats them as unknown and erases each one lazily, the first
 *        time it is allocated.
 *
 *        Formatting therefore stays fast and restartable even on an 8 MB QSPI
 *        NOR: an interrupted format leaves either no device (\c -ENODEV on
 *        the next attach) or a complete one.
 *
 *        Must not be called while the same partition is attached.
 *
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Formatted.
 * \retval -EINVAL
 *         \p config is malformed.
 * \retval -EACCES
 *         \p ikm_key_id is missing, or lacks derive permission for
 *         HKDF-SHA256.
 * \retval -ENOSPC
 *         The partition holds fewer blocks than UBI needs, or more than the
 *         build-time limits allow.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_device_format(const struct ubi_config *config);

/**
 * \brief Attach a formatted partition.
 *
 *        Scans every block once, verifies the CMAC on each header it finds,
 *        rebuilds the logical-to-physical map in RAM and finally consults the
 *        freshness callback.
 *
 *        This function never writes to or erases the flash. A wrong key
 *        therefore fails every header, returns \c -EBADMSG and leaves the
 *        device untouched; retrying with the correct key succeeds.
 *
 * \param[in,out] ubi                   Storage of \ref ubi_device_size bytes.
 * \param[in] config                    Partition, key handle and callbacks.
 *
 * \retval 0
 *         Attached.
 * \retval -EINVAL
 *         \p config is malformed, or the partition geometry disagrees with
 *         the one recorded in the volume table.
 * \retval -EACCES
 *         \p ikm_key_id is missing, or lacks derive permission for
 *         HKDF-SHA256.
 * \retval -EBUSY
 *         \p ubi is already attached.
 * \retval -ENODEV
 *         No volume table found: this is not a UBI device. Call
 *         \ref ubi_device_format if that is expected.
 * \retval -EBADMSG
 *         A volume table was found but failed authentication: wrong key, or
 *         the metadata was modified.
 * \retval -ENOSPC
 *         The partition exceeds the build-time block or volume limits.
 * \retval -EROFS
 *         The freshness callback returned #UBI_FRESHNESS_REJECT.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Detach a device and destroy its derived keys.
 *
 *        No data is lost: everything UBI needs is already on the flash.
 *        Blocks queued for reclaim stay queued and are erased after the next
 *        attach. The key handle supplied in \ref ubi_config is left alone.
 *
 * \param[in,out] ubi                   Attached device.
 *
 * \retval 0
 *         Detached.
 * \retval -EINVAL
 *         \p ubi is not attached.
 */
int ubi_device_deinit(struct ubi_device *ubi);

/**
 * \brief Read geometry and block accounting.
 *
 *        Served entirely from RAM.
 *
 * \param[in] ubi                       Attached device.
 * \param[out] info                     Receives the device state.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p info is \c NULL.
 */
int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);

/**@}*/

/** \defgroup ubi-api-volume UBI volume management
 * @{
 */

/**
 * \brief Create a volume and persist it in the volume table.
 *
 *        Reserves \p leb_count logical blocks but allocates no physical ones;
 *        blocks are taken only when a LEB is first mapped or written. The
 *        assigned identifier is never reused, even after the volume is
 *        removed.
 *
 *        The volume table is rewritten atomically, so an interruption leaves
 *        either the old table or the new one.
 *
 * \param[in,out] ubi                   Attached device.
 * \param[in] config                    Name and size.
 * \param[out] vol_id                   Assigned identifier, or \c NULL.
 *
 * \retval 0
 *         Created.
 * \retval -EINVAL
 *         \p ubi is not attached, or the name is empty or too long.
 * \retval -EEXIST
 *         A volume with that name already exists.
 * \retval -ENOSPC
 *         Not enough logical blocks left, or the volume limit is reached.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_volume_create(struct ubi_device *ubi,
		      const struct ubi_volume_config *config,
		      uint32_t *vol_id);

/**
 * \brief Remove a volume and release its blocks.
 *
 *        The blocks are queued for #UBI_MAINTENANCE_RECLAIM rather than
 *        erased inline, so this call stays fast. Until they are erased their
 *        contents remain readable to anyone with raw flash access.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume to remove.
 *
 * \retval 0
 *         Removed.
 * \retval -EINVAL
 *         \p ubi is not attached.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_volume_remove(struct ubi_device *ubi, uint32_t vol_id);

/**
 * \brief Look up a volume identifier by name.
 *
 *        Identifiers are not stable across a \ref ubi_device_format, so
 *        resolve by name after every attach.
 *
 * \param[in] ubi                       Attached device.
 * \param[in] name                      NUL-terminated volume name.
 * \param[out] vol_id                   Identifier of the volume found.
 *
 * \retval 0
 *         Found.
 * \retval -EINVAL
 *         \p ubi is not attached, or an argument is \c NULL.
 * \retval -ENOENT
 *         No volume with that name.
 */
int ubi_volume_find(struct ubi_device *ubi, const char *name,
		    uint32_t *vol_id);

/**
 * \brief Read a volume's properties.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume to inspect.
 * \param[out] info                     Receives the volume properties.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p info is \c NULL.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info);

/**@}*/

/** \defgroup ubi-api-leb UBI logical erase block operations
 * @{
 */

/**
 * \brief Give a logical erase block a physical one, without writing data.
 *
 *        Required before \ref ubi_leb_write_at. Not required before
 *        \ref ubi_leb_change, which allocates on its own. Mapping an already
 *        mapped LEB succeeds and changes nothing.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Mapped.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p lnum is out of range.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         No physical block available.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Detach a logical erase block from its physical one.
 *
 *        The block is queued for #UBI_MAINTENANCE_RECLAIM. Reads of the LEB
 *        afterwards return erased bytes. Unmapping an unmapped LEB succeeds
 *        and changes nothing.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 *
 * \retval 0
 *         Unmapped.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p lnum is out of range.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum);

/**
 * \brief Read from a logical erase block.
 *
 *        The hot path: one flash read, no metadata access, no checksums,
 *        because the mapping is already in RAM. Setting
 *        \c CONFIG_UBI_VERIFY_ON_READ adds a header re-read and CMAC check
 *        before every call, at a real cost in throughput.
 *
 *        UBI does not track how much of a LEB has been written. Reading past
 *        the written region, or reading an unmapped LEB, fills \p buffer with
 *        erased bytes and succeeds.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param offset                        Byte offset within the LEB.
 * \param[out] buffer                   Destination buffer.
 * \param length                        Bytes to read; \p offset + \p length
 *                                      must not exceed the LEB size.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, or the range
 *         spills past the end of the LEB.
 * \retval -ENOENT
 *         No such volume.
 * \retval -EBADMSG
 *         Only with \c CONFIG_UBI_VERIFY_ON_READ: the header failed its CMAC.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		 uint32_t offset, void *buffer, size_t length);

/**
 * \brief Replace the contents of a logical erase block atomically.
 *
 *        Writes to a freshly allocated physical block and switches the
 *        mapping only once the data is safely down. A power loss at any point
 *        leaves the LEB holding either its previous contents or the new ones,
 *        never a mixture and never a partial write. The old block is queued
 *        for reclaim.
 *
 *        Prefer this whenever a whole block is being rewritten. It costs an
 *        erase only when the free pool has run dry.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write, at most the LEB size.
 *                                      Zero leaves the LEB mapped and empty.
 *
 * \retval 0
 *         The new contents are durable.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, or \p length
 *         exceeds the LEB size.
 * \retval -ENOENT
 *         No such volume.
 * \retval -ENOSPC
 *         No physical block available.
 * \retval -EIO
 *         Flash driver failure; the LEB still holds its previous contents.
 */
int ubi_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		   const void *buffer, size_t length);

/**
 * \brief Append to a logical erase block at a caller-chosen offset.
 *
 *        A direct write to flash. UBI performs no bookkeeping and promises
 *        nothing beyond placing the bytes where asked; the caller owns the
 *        offset.
 *
 *        The contract:
 *        - \p offset and \p length must both be multiples of the write block
 *          size reported by \ref ubi_device_get_info.
 *        - The same region must never be written twice without an
 *          intervening \ref ubi_leb_change or \ref ubi_leb_unmap. On NOR
 *          flash the second write silently corrupts the first and UBI will
 *          not catch it.
 *        - A power loss mid-append leaves a partial record; detecting that
 *          belongs to the caller.
 *        - UBI stores no length, so nothing distinguishes written bytes from
 *          erased ones and the write frontier is not recovered after a
 *          reboot.
 *
 *        Use it for append-only structures such as journals, logs and
 *        key-value stores, where the layer above already tracks a frontier.
 *        For anything else \ref ubi_leb_change is safer and usually just as
 *        fast.
 *
 * \param[in,out] ubi                   Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param offset                        Byte offset within the LEB.
 * \param[in] buffer                    Data to write.
 * \param length                        Bytes to write.
 *
 * \retval 0
 *         The bytes were written.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, the range spills
 *         past the end of the LEB, or the alignment rule was broken.
 * \retval -ENOENT
 *         No such volume, or the LEB is not mapped; call \ref ubi_leb_map
 *         first.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_leb_write_at(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint32_t offset, const void *buffer, size_t length);

/**
 * \brief Read the mapping state of a logical erase block.
 *
 *        Served from RAM.
 *
 * \param[in] ubi                       Attached device.
 * \param vol_id                        Volume.
 * \param lnum                          Logical erase block number.
 * \param[out] info                     Receives the block state.
 *
 * \retval 0
 *         Success.
 * \retval -EINVAL
 *         \p ubi is not attached, \p lnum is out of range, or \p info is
 *         \c NULL.
 * \retval -ENOENT
 *         No such volume.
 */
int ubi_leb_get_info(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     struct ubi_leb_info *info);

/**@}*/

/** \defgroup ubi-api-maintenance UBI maintenance
 * @{
 */

/**
 * \brief Perform deferred housekeeping, up to a caller-set budget.
 *
 *        UBI runs no background thread. Erasing and relocating happen here,
 *        when the application decides the device can afford the latency: one
 *        64 KB erase on QSPI NOR takes on the order of 200 ms, so a budget of
 *        one is a reasonable slice.
 *
 *        Relocation moves data byte for byte and is invisible to callers; the
 *        LEB reads back identically before and after.
 *
 * \param[in,out] ubi                   Attached device.
 * \param operation                     Work to perform.
 * \param budget                        Maximum operations to perform. Zero
 *                                      only reports what is pending.
 * \param[out] result                   Work done and remaining, or \c NULL.
 *
 * \retval 0
 *         Success, including when there was nothing to do.
 * \retval -EINVAL
 *         \p ubi is not attached, or \p operation is unknown.
 * \retval -ENOSPC
 *         Relocation found no spare block to move data into.
 * \retval -EIO
 *         Flash driver failure.
 */
int ubi_maintenance(struct ubi_device *ubi, enum ubi_maintenance_op operation,
		    uint32_t budget, struct ubi_maintenance_result *result);

/**@}*/

#ifdef __cplusplus
}
#endif

#endif /* UBI_H */
