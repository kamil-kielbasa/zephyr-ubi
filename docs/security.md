# Security

UBI authenticates its own metadata, the block headers and the volume table,
with a key that never leaves the PSA key store. Changes made to the metadata
outside UBI are detected, and the application gets counters it can trust to
detect a rollback of the flash. The data in the volumes is stored as written,
neither encrypted nor authenticated.

## How the protection works

1. The application gives UBI the identifier of a key in the PSA key store.
   UBI uses the key only through PSA and never reads it.
2. From that key, UBI derives two keys of its own with HKDF-SHA256: one for
   the block headers and one for the volume table. They exist only while the
   device is attached.
3. Every erase counter header and every volume identifier header carries a
   message authentication code (MAC). It covers the fields of the header and
   the number of the block the header is written in. The volume table carries
   a MAC of its own.
4. Whenever UBI reads a header, it checks the CRC first and the MAC second. A
   failed CRC means accidental damage, such as a write cut short. A valid CRC
   with a failed MAC means the header was changed on purpose, or written under
   another key.

The MAC is AES-CMAC: it is small and fast, and most microcontrollers have AES
in hardware. Headers are verified at attach. With `CONFIG_UBI_VERIFY_ON_READ`,
`ubi_leb_read()` also verifies the volume identifier header before reading,
which catches changes made while the device runs. The byte layout is in
[On-flash format](on-flash-format.md#mac-and-crc).

## Threat model

The protection is designed against an attacker who:

- can read and write the flash that holds the partition, for example after
  desoldering the chip, by clipping onto its bus, or through an update meant
  for another partition of the same flash;
- can power the device on and off at any moment, and observe it while it
  runs.

The attacker cannot read or use the key. The key stays in the PSA key store,
for example in TF-M or a secure element, and only the device's own firmware
can use it: the attacker cannot run code of its own on the device.

Out of scope are side-channel and fault-injection attacks, code running in
the secure domain, and denial of service: whoever can write the flash can
always erase it.

## What is detected

| Change made to the flash | Detected by | How |
|---|---|---|
| A header modified | UBI | The MAC of the header fails: `UBI_EVENT_HDR_TAMPERED`. |
| A volume table copy modified | UBI | The copy fails verification and the other copy is used: `UBI_EVENT_VOLUME_TABLE_CORRUPT`. |
| A genuine block copied to another position | UBI | The MAC covers the block number, so the copy fails it. |
| A block taken from another device or from an earlier format | UBI | The MAC depends on the device's key and covers `image_seq`; such a block is never used. |
| Data in a volume modified | Application | Its own authenticated encryption; see [Protecting your data](#protecting-your-data). |
| Data moved to another LEB | Application | The volume and the LEB number in its authenticated data. |
| A block erased | Application | The LEB reads back as erased. |
| The whole flash restored from an earlier copy | Application | The state callback; see [Rollback detection](#rollback-detection). |

Blocks that fail verification are never used. A damaged block that still
holds data is kept, unused, until `UBI_MAINTENANCE_DISCARD`; see
[Damaged headers](how-it-works.md#damaged-headers). What to do on each event
is in [Operations](operations.md#events).

## Limits

UBI cannot tell an older genuine version of a block from the current one. An
attacker who saved a block earlier can write it back to the same place: its
MAC verifies, its block number and image match, and the volume table has not
changed. The same applies when the attacker erases the current copy of a
block and an older copy, not yet erased by UBI, takes its place. Detecting
this would require a record of every write in trusted storage.

Two practices narrow this gap:

- Run `UBI_MAINTENANCE_RECLAIM` regularly, so that old copies do not linger
  on the flash.
- Where old data must never be accepted again, keep its version number in
  trusted storage and check it as part of the data's authentication.

A restore of the whole flash is a different case: the counters in the next
section can reveal it.

## Rollback detection

An earlier state of the flash is as genuine as the current one, so UBI cannot
reject it by itself. The application can, if it remembers where the device
was. At every attach, and every `CONFIG_UBI_STATE_CHECK_INTERVAL` flash
writes, UBI passes `struct ubi_device_info` to the state callback, which
decides whether to trust the device. A verdict of `UBI_STATE_UNTRUSTED` fails
the attach with `-EROFS`. On an attached device, it makes every write fail
with `-EROFS` until the next attach, while reads still work.

Good practice:

- Keep the last trusted values of the counters below in storage the attacker
  cannot roll back, such as PSA Internal Trusted Storage, a monotonic counter
  or a secure element.
- Save the new values before the callback returns `UBI_STATE_TRUSTED`.
- Before formatting on purpose, set a flag in the same storage, so that the
  callback accepts the new image once.
- After creating, resizing or removing a volume, save the new `revision` from
  `ubi_device_get_info()` at once, rather than at the next check.

| Counter | In normal use |
|---|---|
| `image_seq` | Drawn at random by each format; never changes otherwise. |
| `revision` | Grows with every volume create, resize and remove, and every volume table repair. Never goes down. |
| `max_sqnum` | Grows with every header written. After a reboot it may be slightly lower, if the newest headers were erased. |
| `healthy_pebs` | Grows as reclaim prepares blocks after a format. Goes down when a header is damaged, a write is cut short or a block is retired. |
| `total_erase_count` | Grows with every erase. Goes down only by the erase counts of blocks that left `healthy_pebs`. |

`image_seq` and `revision` never go back, so compare them exactly. They catch
a flash restored from before the last format or the last change to the
volumes. A copy taken after that has the same values, but lower `max_sqnum`
and `total_erase_count`, since these grow with every write and erase. Both
may drop a little in normal use, so compare them within a tolerance.

The whole check, in pseudocode:

```text
# Tolerances, set from how the application uses the device
SQNUM_SLACK   = newest headers it may erase before writing again
HEALTHY_SLACK = blocks that may lose their header between two checks

on_state(info):
    kept = trusted_store.load()

    if kept is empty:                         # nothing stored yet
        return accept(info)

    if info.image_seq != kept.image_seq:      # another image
        if not kept.format_requested:
            return UNTRUSTED
        return accept(info)

    if info.revision < kept.revision:         # an older volume layout
        return UNTRUSTED

    if info.max_sqnum + SQNUM_SLACK < kept.max_sqnum:
        return UNTRUSTED                      # headers written since are gone

    lost = max(0, kept.healthy_pebs - info.healthy_pebs)
    if lost > HEALTHY_SLACK:
        return UNTRUSTED                      # too many blocks lost at once

    # A lost block takes its erase count out of the total.
    if info.total_erase_count + lost * kept.max_erase_count
            < kept.total_erase_count:
        return UNTRUSTED                      # erases made since are gone

    return accept(info)

accept(info):
    trusted_store.save(info.image_seq, info.revision, info.max_sqnum,
                       info.healthy_pebs, info.total_erase_count,
                       info.max_erase_count, format_requested = false)
    return TRUSTED
```

Without trusted storage, a rollback cannot be detected. The callback then
returns `UBI_STATE_TRUSTED` every time, and the code shows that choice.

## Protecting your data

UBI stores the data in the volumes as it is written. Anyone who can read the
flash can read it, and anyone who can write the flash can change it. The
checksum UBI keeps for data written with `ubi_leb_change()` detects
accidental damage, not deliberate changes. If the data needs protection, add
it in the application:

- Encrypt and authenticate the data with an authenticated cipher, under a key
  held in the PSA key store. That key can be derived from the key you give
  UBI, under a label of your own.
- Include the volume name and the LEB number in the authenticated data, so
  that a block moved elsewhere no longer verifies.
- Never use a nonce twice with the same key. `ubi_leb_change()` rewrites a
  LEB from its start, so a nonce based on the position in the LEB would
  repeat; take it from a counter or a random source, and store it with the
  data.
- Where old contents must not be accepted again, add a version number kept
  in trusted storage; see [Limits](#limits).

Once a LEB has contents, `ubi_leb_change()` replaces them atomically, so a
protected record is always read back whole: the old one or the new one.
