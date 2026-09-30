# Security

## Assumptions

The attacker can **read and write the raw flash**, through a desoldered chip,
an SPI clip, a debug port or a malicious update to another partition, and can
boot the device and watch it run.

The attacker does **not** have the input keying material. It stays in the PSA
key store (TF-M, a secure element, whatever the platform provides); UBI is
handed a key handle and derives its own keys from it.

Out of scope: side channels, fault injection, code running inside the secure
domain, and availability. Whoever can write the flash can always erase it.

## What is caught, and by whom

| # | attack | caught by | how |
|---|---|---|---|
| 1 | a byte changed in either header | UBI | the header MAC |
| 2 | a byte changed in your data | you | your own AEAD |
| 3 | a whole block copied elsewhere | UBI | `pnum` inside the MAC's message |
| 4 | data moved between logical blocks | you | `vol_id` and `lnum` in your AAD |
| 5 | a block from another image inserted, a volume table copy included | UBI | `image_seq`, authenticated |
| 6 | the whole flash image rolled back | your application | the state callback sees `image_seq` change or `revision` go down |
| 7 | the volume table replaced | UBI | the record MAC |
| 8 | a block erased | you | the logical block reads back empty |
| 9 | **one block restored to an older authentic version, same `pnum`** | **nobody** | see below |

## The boundary

Row 9 is where this stops. Every MAC verifies, `pnum` and `image_seq` match,
and `revision` does not move, because the volume table did not change. Closing
it would take a write to a trusted store for every UBI write. UBIFS
authentication in Linux draws the line in the same place.

An old but authentic copy left on the flash is material for this attack, so
run `UBI_MAINTENANCE_RECLAIM`: it erases them.

## Rollback detection

The state callback receives `struct ubi_device_info`. Anchored where the
attacker cannot rewind them (PSA ITS, a monotonic counter, a secure element),
its counters show the flash going back in time. Each goes down in normal use
only as listed:

| counter | goes down in normal use |
|---|---|
| `image_seq` | never; it changes only at a format |
| `revision` | never within one `image_seq` |
| `max_sqnum` | across a reboot, once the blocks carrying the highest numbers are erased |
| `healthy_pebs` | when a header is damaged, a write is cut short or a block is retired |
| `total_erase_count` | only when a block leaves `healthy_pebs` |

`image_seq` and `revision` catch the flash put back to a copy taken before the
last layout change, or to another image. A copy taken since keeps both; only
`max_sqnum` and `total_erase_count` show it, lower by the headers written and
the erases made since. A check built on them has to allow for their drops in
normal use.

The rule the sample uses: the same image, at a revision no older than the last
one, and a new image only after a format the application asked for itself.

```c
static enum ubi_state_verdict on_state(const struct ubi_device_info *info,
				       void *user_context)
{
	struct anchor *kept = user_context;	/* read from the trusted store */

	if (kept->anchored && !kept->format_authorised) {
		if (info->image_seq != kept->image_seq)
			return UBI_STATE_UNTRUSTED;

		if (info->revision < kept->revision)
			return UBI_STATE_UNTRUSTED;
	}

	/* Commit these to the trusted store before returning. */
	kept->anchored = true;
	kept->format_authorised = false;
	kept->image_seq = info->image_seq;
	kept->revision = info->revision;

	return UBI_STATE_TRUSTED;
}
```

The callback runs at attach and every `CONFIG_UBI_STATE_CHECK_INTERVAL` flash
writes. To keep the anchor exact, also record `revision` from
`ubi_device_get_info()` after your own create, resize and remove calls.

An application with nowhere trusted to keep the anchor returns
`UBI_STATE_TRUSTED` every time: rollback then goes undetected, by a decision
visible in the code.

## One key per partition

The MAC binds a header to its block number, not to its partition. Two
partitions under the same keys accept each other's blocks. Give each a
`key_context` of its own, such as its name, or keying material of its own.

## Your data

**UBI does not encrypt data**, and authenticates only its own metadata. Anyone
with the raw flash reads volume names, sizes, erase counts and contents.

Protect data with an AEAD of your own:

> **Bind it to `vol_id` and `lnum`, and use a new nonce for every version of
> a block.**

`ubi_leb_change()` starts a block again from offset zero, so a nonce tied to
the offset alone repeats. In GCM a repeated nonce leaks the authentication
subkey.

## Damage and tampering

Every header carries a MAC and then a CRC over everything before it, the MAC
included. A torn write fails the CRC and is reported as
`UBI_EVENT_HDR_CORRUPT`; a changed field with a recomputed CRC fails the MAC
and is reported as `UBI_EVENT_HDR_TAMPERED`.

The volume table record has one event, `UBI_EVENT_VOLUME_TABLE_CORRUPT`: its
checksum sits in the sealed header in front of it, so whoever could fix that
checksum already holds the key.

The data checksum tells a change cut short from a whole one, not tampering
from damage. Relocation seals a copy afresh, so damage that set in before the
move travels with it; your AEAD catches it.

## Damaged blocks are kept

A block whose erase counter header verifies but whose volume identifier header
does not, over data that is not blank, is kept as it is: counted in
`corrupt_pebs`, never allocated, erased or moved until
`UBI_MAINTENANCE_DISCARD`. Attach refuses the partition once such blocks reach
a twentieth of its good blocks, rounded down, or eight when that is zero: a
partition that damaged is probably not the one it appears to be.
