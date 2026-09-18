# API walkthrough

Every usage scenario, in code. Full contracts live in
[include/ubi/ubi.h](../include/ubi/ubi.h). For how UBI works underneath, see
`doc/how-it-works.md`.

---

## 1. The device handle

`struct ubi_device` is **opaque** — its layout belongs to the library. You allocate the
storage; the size comes from the library at run time:

```c
#include <ubi/ubi.h>

struct ubi_device *ubi = malloc(ubi_device_size());
```

The handle itself is small and its size does not depend on how large the managed
flash is: everything that scales with the partition — one byte of state, four bytes of
erase count and two bytes of mapping per physical erase block — is taken from the heap
when the device attaches and given back when it detaches. Size the heap with
`CONFIG_HEAP_MEM_POOL_SIZE`; `ubi_device_init()` returns `-ENOMEM` when it is too small
for the partition it was pointed at.

---

## 2. The key

UBI does not accept raw key material. It takes a **PSA key handle** and derives its own
keys from it with HKDF-SHA256; the bytes never cross the API boundary.

```c
static psa_key_id_t device_ikm_key(void)
{
	/* Usually the key already exists: loaded from KMU, derived from the HUK,
	 * or installed during production. Then you simply reference it. */
	return DEVICE_IKM_KEY_ID;
}
```

Requirements: `PSA_KEY_USAGE_DERIVE` and a policy permitting
`PSA_ALG_HKDF(PSA_ALG_SHA_256)`. Either one missing gives `-EACCES`.

**The key must be unique per device.** Derivation is salted with a 32-bit image sequence
number, so a fleet sharing one key will eventually hit a collision — and then two devices
hold identical header keys, and a PEB moved between them would verify.

---

## 3. Bringing the device up

UBI never formats a partition on its own initiative. If it finds no volume table it says
`-ENODEV` and leaves the flash untouched.

```c
static const struct ubi_config cfg = {
	.flash_area_id = PARTITION_ID(storage_partition),
	.ikm_key_id    = DEVICE_IKM_KEY_ID,
	.event_cb      = on_ubi_event,
	.state_cb      = on_ubi_state,
};

int storage_init(void)
{
	int ret = ubi_device_init(ubi, &cfg);

	if (ret == -ENODEV) {
		LOG_INF("Blank partition, formatting");
		ret = ubi_device_format(&cfg);
		if (ret == 0) {
			ret = ubi_device_init(ubi, &cfg);
		}
	}
	return ret;
}
```

What the other codes mean:

| code | what happened | what to do |
|---|---|---|
| `-EBADMSG` | a volume table is there but fails authentication | wrong key, or someone touched the metadata — **do not reflexively format** |
| `-EACCES` | the key is missing or does not permit derivation | a provisioning problem, not a flash problem |
| `-EROFS` | your trust callback returned `UBI_STATE_UNTRUSTED` | rollback detected |
| `-EINVAL` | partition geometry disagrees with the recorded one | the partition was resized |
| `-EIO` | the flash driver failed | hardware problem |

`-EBADMSG` deserves care. Attach performs **not a single write**, so a mistaken key
destroys nothing — supply the right handle and the device comes back. Formatting
automatically in that branch would turn a typo into permanent data loss.

---

## 4. Volumes

Identifiers are not stable across a format, so resolve your volume by name after every
attach.

```c
static uint32_t cfg_vol;

int config_volume_open(void)
{
	int ret = ubi_volume_find(ubi, "config", &cfg_vol);

	if (ret == -ENOENT) {
		const struct ubi_volume_config vc = {
			.name      = "config",
			.leb_count = 8,
		};
		ret = ubi_volume_create(ubi, &vc, &cfg_vol);
	}
	return ret;
}
```

Creating a volume reserves logical blocks but claims no physical ones. Blocks leave the
pool only on the first write.

Every volume is dynamic, so that reservation is a claim on the shared pool rather than a
fence. `ubi_volume_resize` takes more of what is left or hands blocks back:

```c
int config_volume_grow(uint32_t leb_count)
{
	return ubi_volume_resize(ubi, cfg_vol, leb_count);
}
```

Growing is refused with `-ENOSPC` when the pool has less left than you ask for. Shrinking
is refused with `-EBUSY` while any logical block above the new size is still mapped, so a
mistyped size cannot cost you data — unmap the tail first if that is what you meant.

---

## 5. Whole-block writes — `ubi_leb_change`

The default way to write. Atomic: a power loss leaves either the old contents or the new
ones.

```c
int config_save(const struct app_config *data)
{
	return ubi_leb_change(ubi, cfg_vol, 0, data, sizeof(*data));
}
```

No `map`, no `erase`, no `sync`. `change` takes a free block, writes it, switches the
mapping and hands the old block back for reclaim. An erase usually **does not happen** on
this path, because the free pool is kept erased and stamped ahead of time.

---

## 6. Append writes — `ubi_leb_write_at`

For journals, logs and key-value stores, where the layer above already tracks its own
write frontier. UBI keeps no bookkeeping here — it trusts you completely.

```c
struct journal {
	uint32_t lnum;
	uint32_t offset;   /* the frontier is yours, not UBI's */
};

int journal_append(struct journal *j, const void *record, size_t length)
{
	struct ubi_device_info info;

	ubi_device_get_info(ubi, &info);

	/* offset and length must both be multiples of write_block_size */
	size_t padded = ROUND_UP(length, info.write_block_size);

	if (j->offset + padded > info.leb_size) {
		return -ENOSPC;          /* block full - move to the next one */
	}

	int ret = ubi_leb_write_at(ubi, log_vol, j->lnum, j->offset, record, padded);

	if (ret == 0) {
		j->offset += padded;
	}
	return ret;
}
```

The block has to exist before the first `write_at`:

```c
ubi_leb_map(ubi, log_vol, j->lnum);
```

Three things UBI will **not** do for you:

- **It will not catch a double write.** On NOR flash the second write to the same region
  returns success and silently corrupts the first. Guarding the offset is your job.
- **It will not recover the frontier after a reboot.** UBI stores no length, so you must
  scan the block yourself or keep the offset in your own metadata.
- **It will not detect a torn record.** A power loss mid-append leaves a fragment; your
  record format has to recognise it.

This is the same bargain NVS makes, and the same one Linux's `ubi_leb_write` makes. If any
of those three worries you, use `ubi_leb_change`.

---

## 7. Reading

The fastest path in the library: one `flash_area_read`, no metadata, no checksums.

```c
int config_load(struct app_config *out)
{
	return ubi_leb_read(ubi, cfg_vol, 0, 0, out, sizeof(*out));
}
```

Reading past the written region, or reading an unmapped LEB, fills the buffer with erased
bytes and **returns success**. UBI does not know how much you put there — if that matters,
record it yourself.

---

## 8. Maintenance

There is no background thread. Erasing and relocating happen only when you ask, and only
as much as you allow.

```c
void storage_idle_work(void)
{
	struct ubi_device_info info;
	struct ubi_maintenance_result res;

	ubi_device_get_info(ubi, &info);

	if (info.free_pebs < 4 && info.reclaimable_pebs > 0) {
		ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &res);
	}

	if (info.max_erase_count - info.min_erase_count > 128) {
		ubi_maintenance(ubi, UBI_MAINTENANCE_RELOCATE, 1, &res);
	}

        if (volume_table_degraded) {
                ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &res);
        }
{
	switch (e->type) {
	case UBI_EVENT_HDR_CORRUPT:
		/* CRC mismatch - an interrupted write or bit rot. */
		LOG_WRN("Corrupt header, PEB %u", e->pnum);
		break;

	case UBI_EVENT_HDR_TAMPERED:
		/* CRC matches, CMAC does not - someone recomputed the checksum. */
		LOG_ERR("Tampering, PEB %u", e->pnum);
		security_incident();
		break;

	case UBI_EVENT_VOLUME_TABLE_CORRUPT:
		/* One copy of the layout is gone. Whether it rotted or was
		 * forged cannot be told apart: its checksum is sealed. */
		LOG_ERR("Volume table copy lost, PEB %u", e->pnum);

        case UBI_EVENT_VOLUME_TABLE_DEGRADED:
                /* One copy left: one erase would now cost a revision. */
                LOG_WRN("Volume table degraded, repair is due");
                volume_table_degraded = true;
                break;

        case UBI_EVENT_LEB_ORPHANED:
                LOG_WRN("PEB %u claims volume %u block %u, which is gone",
                        e->pnum, e->vol_id, e->lnum);
                break;

        case UBI_EVENT_PEB_BAD:
                LOG_WRN("PEB %u retired", e->pnum);

## 10. Rollback detection

```c
static enum ubi_state_verdict on_ubi_state(const struct ubi_device_info *info,
					   void *user_context)
{
	struct rollback_anchor anchor;

	if (trusted_store_load(&anchor) != 0) {
		trusted_store_save(info);	    /* first boot */
		return UBI_STATE_TRUSTED;
	}

	if (info->revision < anchor.revision ||
	    info->global_sqnum < anchor.global_sqnum) {
		return UBI_STATE_UNTRUSTED;
	}

	trusted_store_save(info);
	return UBI_STATE_TRUSTED;
}
```

UBI asks once at the end of every attach, and again every
`CONFIG_UBI_STATE_CHECK_INTERVAL` metadata writes, so a long uptime is not a way
around the check. It asks *before* the write it is about to make, which is what
lets a refusal be honoured with nothing on the flash.

Save the new values before returning `UBI_STATE_TRUSTED`: UBI carries on the
moment you return.

`UBI_STATE_UNTRUSTED` is final. The attach that provoked it fails with `-EROFS`,
and on an attached device every operation that would write returns `-EROFS` from
then on. Only detaching and attaching again clears it, so carrying on is a
deliberate act rather than the result of a retry. Reads and
`ubi_device_get_info()` keep working, so you can still report what happened and
salvage what you need.

The anchor has to live outside the flash UBI manages — PSA ITS, an RPMC counter, a secure
element. Without that there is nothing to compare against.

**Scope:** this catches a rollback of the **whole device**. It does not catch a rollback
of a single LEB — restoring one PEB to an older authentic image leaves the global maximum
untouched. Closing that gap would need per-LEB state in the trusted store, updated on
every write. Linux UBIFS declares exactly the same limitation.

---

## 11. What you must do yourself, if your data needs protecting

UBI authenticates **its** headers and **its** volume table. Your data it stores verbatim.
If it has to be confidential or tamper-evident, seal it before handing it over.

Two rules:

**Bind your AEAD to `vol_id` and `lnum`.** You know both, because you pass them in.
Without that, an attacker moves a sealed block from one LEB to another and the tag still
verifies.

**Change the salt for every new content.** `ubi_leb_change` resets the block and you write
from offset 0 again. If the nonce does not change, you repeat it — and in AES-GCM a
repeated nonce lets an attacker recover the authentication key `H` and forge any tag. That
is not a loss of confidentiality, it is a total break.

```c
int config_save_sealed(const struct app_config *plain)
{
	uint8_t salt[8];
	uint8_t sealed[SEALED_LEN];

	psa_generate_random(salt, sizeof(salt));       /* fresh salt on every write */

	/* nonce = salt || counter, AAD = vol_id || lnum */
	app_seal(sealed, plain, salt, cfg_vol, 0);

	return ubi_leb_change(ubi, cfg_vol, 0, sealed, sizeof(sealed));
}
```

Keep the salt in the clear at the head of your record — it does not need to be secret, it
needs to be fresh.

---

## 12. Shutting down

```c
ubi_device_deinit(ubi);
```

Destroys the keys UBI derived and releases the handle. The IKM handle you supplied in the
configuration is **left alone** — it is yours. Nothing is lost: everything UBI needs is
already on the flash, and blocks awaiting reclaim are erased after the next attach.
