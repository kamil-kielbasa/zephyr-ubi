# Security

## What is assumed

The attacker can **read and write the raw flash**: a desoldered chip, an SPI
clip, a debug port, a malicious update to another partition, DMA. They can boot
the device and watch it run.

The attacker does **not** have the input keying material. It lives in the PSA
key store — TF-M, a CryptoCell, whatever the platform provides — and never
leaves that domain. UBI is handed a key handle and derives from it; no raw key
material crosses the API in either direction.

Out of scope: side channels, fault injection, code execution inside the secure
domain, and availability. Anyone who can write the flash can always erase it.

## What is caught, and by whom

| # | attack | caught by | how |
|---|---|---|---|
| 1 | a byte changed in either header | UBI | the header tag |
| 2 | a byte changed in your data | you | your own AEAD |
| 3 | a whole block copied elsewhere | UBI | `pnum` inside the tag's message |
| 4 | data moved between logical blocks | you | `vol_id` and `lnum` in your AAD |
| 5 | a block from another image inserted | UBI | `image_seq`, authenticated |
| 6 | the whole flash image rolled back | your application | the state callback sees `revision` or `global_sqnum` go backwards |
| 7 | the volume table replaced | UBI | the record tag |
| 8 | a block erased | you | the logical block reads back empty |
| 9 | **one block restored to an older authentic version, same `pnum`** | **nobody** | see below |

## The boundary

Row 9 is where this stops. Every tag still verifies, because it verified once;
`pnum` matches; `image_seq` matches; and the global maximum sequence number
does not move, because other blocks hold it.

Closing it would need a trusted store updated as often as data is written — a
Merkle tree over the mapping, or a per-block counter in PSA ITS. On the parts
this targets that is one trusted-store write per UBI write, which is not a
trade worth making.

Linux draws the line in the same place. From
`Documentation/filesystems/ubifs-authentication.rst`:

> UBIFS authentication will not protect against rollback of full flash
> contents. […] It will also not protect against partial rollback of
> individual index commits. […] This is further helped by the wear-leveling
> operations of UBI which copies contents from one physical eraseblock to
> another and does not atomically erase the first eraseblock.

That last sentence is why reclaiming blocks matters for more than capacity: an
old but still authentic copy left lying on the flash is free material for an
attacker. Run `UBI_MAINTENANCE_RECLAIM`.

## Rollback detection is yours to wire up

UBI reports `revision` and `global_sqnum` to the state callback and does as it
is told. Anchoring them somewhere the attacker cannot rewind — PSA ITS, an RPMC
counter, a secure element — is the application's job, because only the
application knows what it has.

An application with nowhere trustworthy to keep them returns
`UBI_STATE_TRUSTED` every time. Rollback then goes undetected, but the decision
is visible in the code rather than implied by a zeroed field.

## What UBI does not do

**It does not encrypt your data.** Only metadata is authenticated. Anyone with
raw flash access reads volume names, sizes, erase counts and block contents.

**It hands you nothing to bind your ciphertext to.** That was considered and
rejected. Binding to a physical position breaks legitimate relocation and adds
no detection, because the position is already covered by the header tag.
Binding to logical identity is redundant: you passed `vol_id` and `lnum` in
yourself. Encryption callbacks would need a staging buffer inside UBI and have
a tangled contract with a caller's AEAD.

So the contract lives here instead:

> **Bind your AEAD to `vol_id` and `lnum`, and manage your own per-version
> nonce.**

The per-version part is not optional. `ubi_leb_change()` resets a logical block
and you write again from offset zero. With a fixed nonce you would repeat it —
and in GCM a repeated nonce does not merely cost confidentiality, it leaks the
authentication subkey and breaks authentication outright.

## Damage versus tampering

Headers carry both a CRC and a tag, and the CRC is computed last, over the tag.
So the two are distinguishable, and reported separately:

- `UBI_EVENT_HDR_CORRUPT` — the CRC failed. An interrupted write or bit rot.
- `UBI_EVENT_HDR_TAMPERED` — the CRC passed and the tag did not. Someone
  changed a field and recomputed the checksum.

The volume table record gets no such split, and that is deliberate. Its CRC
lives in the sealed header in front of it, so anyone able to repair that
checksum already holds the key. A damaged record and a forged one are
indistinguishable from outside, and one event says so:
`UBI_EVENT_VOLUME_TABLE_CORRUPT`.
