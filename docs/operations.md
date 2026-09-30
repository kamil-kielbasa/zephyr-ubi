# Operations

What UBI reports, what a power cut leaves, and what it costs. The contract of
every call is in
[include/ubi/ubi.h](https://github.com/kamil-kielbasa/zephyr-ubi/blob/main/include/ubi/ubi.h).

## When an attach fails

`ubi_device_init()` writes nothing, so a failed attach changes nothing. Only
`-ENODEV` says there is nothing to lose.

| returns | means | do |
|---|---|---|
| `-ENODEV` | no UBI metadata: blank, or someone else's bytes | format, if that is expected here |
| `-EBADMSG` | metadata that will not verify: the wrong key, both volume table copies damaged, or data with no table left | keep the device; a format destroys the data |
| `-ENOTSUP` | written by a newer release, or with more volumes than `CONFIG_UBI_MAX_NR_OF_VOLUMES` | run the newer firmware, or raise the limit |
| `-EIO` | a block could not be read, or the crypto backend failed | try again; if it persists, it is the hardware |
| `-EROFS` | the state callback refused the device | treat it as a rollback |
| `-EINVAL` | the partition is not whole erase blocks of one size, not the geometry formatted, or has too many corrupt blocks | check the devicetree; see [maintenance](#keeping-maintenance-up) |
| `-EACCES` | the key handle is missing or may not derive with HKDF | fix the key's policy |
| `-ENOMEM` | not enough heap | see [Resources](#resources) |
| `-EBUSY` | the partition is attached or being formatted | detach the other handle first |
| `-ENOSPC` | more than 65534 blocks, or a table declaring more blocks than there are | use fewer, larger blocks |

## Events

Events are reports: UBI has acted on what it found before the callback runs.

| event | means | do |
|---|---|---|
| `UBI_EVENT_HDR_CORRUPT` | a header failed its CRC: a write or erase cut short, or bit rot | nothing after a power cut; data behind a bad header is kept, see `corrupt_pebs` |
| `UBI_EVENT_HDR_TAMPERED` | a header passed its CRC and failed its MAC: modified, or sealed under another key | expected after a key change until reclaim has run; otherwise keep the device as evidence |
| `UBI_EVENT_VOLUME_TABLE_CORRUPT` | a volume table copy could not be used | nothing while the other copy carries the device |
| `UBI_EVENT_VOLUME_TABLE_DEGRADED` | the table rests on one copy | `UBI_MAINTENANCE_REPAIR` soon |
| `UBI_EVENT_LEB_ORPHANED` | a block names a volume the table does not describe | nothing; reclaim takes it |
| `UBI_EVENT_PEB_BAD` | a block was retired after a failed write or erase, or an erase count at its limit | after a failed write, `UBI_MAINTENANCE_REPAIR`; after a failed erase the device is read-only |
| `UBI_EVENT_DATA_CORRUPT` | a sealed block fails its data checksum: a change cut short, or damage | nothing if an older copy stood in; otherwise the block is kept as it reads |

## Power loss

| cut short | after the reboot |
|---|---|
| `ubi_leb_change()` | the old contents or the new; a block that had none keeps what reached the flash, with `UBI_EVENT_DATA_CORRUPT` |
| `ubi_leb_write_at()` | what reached the flash; the application finds its own frontier |
| `ubi_leb_erase()`, or an unmap and a reclaim | the last contents or nothing, never contents a change replaced |
| volume create, resize or remove | the old layout or the new |
| `UBI_MAINTENANCE_RELOCATE` | the block where it was or where it was moved |
| `ubi_device_format()` | the earlier device or the new one |

The third row relies on `CONFIG_UBI_ERASE_INVALIDATES_HEADERS`. Without it, an
erase cut short can leave a block with its headers and part of its data:
sealed data is then reported with `UBI_EVENT_DATA_CORRUPT`, appended data is
not.

A write that fails, unlike a power cut, leaves RAM and flash in agreement:
what the device reports right after is what the next attach finds.

## Keeping maintenance up

| operation | when | budget |
|---|---|---|
| `UBI_MAINTENANCE_RECLAIM` | whenever the device is idle | a few blocks; a full free pool makes writes erase-free |
| `UBI_MAINTENANCE_RELOCATE` | `relocatable_pebs` is not zero | one at a time; each moves a block and erases one |
| `UBI_MAINTENANCE_REPAIR` | after `UBI_EVENT_VOLUME_TABLE_DEGRADED` or `UBI_EVENT_PEB_BAD` | until `remaining` is zero |
| `UBI_MAINTENANCE_DISCARD` | once the application has read what it wants from `corrupt_pebs` | all of them |

A budget of zero only counts the work waiting. Attach refuses a partition once
corrupt blocks reach a twentieth of its good blocks, rounded down, or eight
when that is zero; discard before that.

A failed erase leaves the device read-only until the next attach: every call
that would write returns `-EROFS`, reads keep working. A part that keeps
failing erases is at the end of its life.

## Changing the key

Everything UBI writes is sealed under keys derived from the keying material,
so new keying material means a new device:

1. Attach under the old key and copy out what has to survive.
2. Detach and format under the new key.
3. Attach under the new key. Each old block fails its MAC, is reported with
   `UBI_EVENT_HDR_TAMPERED` and counted in `reclaimable_pebs`.
4. Run `UBI_MAINTENANCE_RECLAIM` until `reclaimable_pebs` is zero.
5. Create the volumes and write back what was copied out.

The erase counts were sealed under the old key and are lost with it.

## Resources

Sizes from the nRF5340 build (Cortex-M33, `-Os`, logging off, mbedTLS),
timings on the nRF5340 DK:

| what | how much |
|---|---|
| code | about 11 KB, plus mbedTLS for AES-CMAC, HKDF and SHA-256 |
| handle, `ubi_device_size()` | 328 bytes with 4 volumes, 2248 with 64 |
| heap at attach | 8 bytes per erase block, and a scratch buffer of 392 bytes with 4 volumes, 3480 with 64 |
| heap during a format | a handle and a scratch buffer of its own |
| stack | 1704 bytes at most for any call on a Cortex-M33, with the default `CONFIG_UBI_IO_CHUNK_SIZE`; a larger chunk adds up to its growth |
| attach | 151 ms at most for 64 blocks of 4 KiB on the DK's MX25R64 |
| erase | 87 ms for a 4 KiB block and 1.2 s for a 64 KiB one on the same part |

`CONFIG_UBI_SELF_CHECKS` adds one bit per erase block of heap.

## Limits

- 4 to 65534 erase blocks of one size, each larger than the two headers and
  the volume table record.
- A write block that divides 64 bytes.
- Volume names of 1 to 16 characters.
- Up to `CONFIG_UBI_MAX_NR_OF_VOLUMES` volumes, 64 at most. The option may
  grow from one firmware to the next, never shrink: a build that allows fewer
  volumes than a device holds refuses it with `-ENOTSUP`.
- Erase counts up to 2³¹ − 1.
