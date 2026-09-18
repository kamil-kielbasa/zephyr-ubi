# How it works

A tour of the model, for someone about to read the code or debug a device.

## Logical and physical blocks

A **physical erase block** (PEB) is one erase unit of the flash. A **logical
erase block** (LEB) is what a volume addresses. The mapping between them is
UBI's whole job.

Nothing in the API names a PEB. An application writes to volume 3, block 7; UBI
decides which piece of silicon that lands on, and moves it whenever that serves
the flash better. The mapping is rebuilt from the flash on every attach and
kept in RAM — it is never stored as a table.

A LEB is smaller than a PEB by the 128 bytes the two headers take.

## What a block can be

Every PEB is in one of six states, all of them RAM-only:

| state | meaning |
|---|---|
| `UNKNOWN` | no usable erase counter header; reclaim can make it free |
| `FREE` | erased, stamped with an erase counter, waiting |
| `MAPPED` | carries a logical block |
| `RECLAIM` | its contents are dead; an erase will hand it back |
| `BAD` | refused an operation once, and will get one more chance |
| `WORN_OUT` | refused again; out of service until the next attach |

Retirement lives only in RAM on purpose. A block that failed a write may well
be fine after a power cycle, and nothing is gained by writing that verdict down
where it cannot be revised.

## Attach

Attach reads the flash twice and decides everything else in memory.

The **first pass** judges each block by its headers alone. It cannot yet know
which image a block belongs to, because that lives in the volume table, which
lives in a block this pass is still looking for. So it records erase counts,
tracks the highest sequence number seen, and notes which blocks claim the
reserved volume the table uses.

Between the passes the two copies of the volume table are read and one is
adopted — the newer of the two, by sequence number. If they disagree, or only
one is readable, the device works but reports `UBI_EVENT_VOLUME_TABLE_DEGRADED`
and one erase would now cost a revision instead of being survivable.

The **second pass** applies what the table settled. Blocks stamped for another
image drop back to `UNKNOWN`; the rest are hung off the logical blocks their
headers name. A block claiming a volume or block number the table does not
describe is an orphan: authentic, but pointing at nothing. It is reported and
queued for reclaim.

Attach writes nothing. It costs roughly two reads per block plus one over the
data each sealed block promised, and no erases at all.

### Duplicates

Two blocks can claim the same logical block after a power loss mid-update. The
higher sequence number wins, except in one case: if the newer block is sealed —
`copy_flag` set — and its data does not match the checksum its header carries,
it loses regardless. That is what makes `ubi_leb_change()` atomic across a
power cut. The older copy survives intact because the newer one never became
credible.

## Writing

`ubi_leb_change()` replaces a logical block whole. It takes a free physical
block, writes the header and the data, and only then switches the mapping. The
old block becomes `RECLAIM`. If anything fails before the switch, nothing has
changed.

`ubi_leb_write_at()` appends at an offset you choose, with no seal and no
checksum, and maps the block on demand if it is not mapped yet. It is the cheap
path and it trusts you completely: it will not stop overlapping writes, will
not recover the append frontier after a reboot, and will not notice a torn
record. The same bargain Linux's `ubi_leb_write()` makes.

`ubi_leb_unmap()` drops the mapping in RAM and queues the block. It does not
erase anything and it is **not durable**: the block still carries its header,
so the next attach maps it again. To lose data for good, erase the block with
`ubi_leb_erase()` or overwrite it.

## Pools

The free pool is what writes draw from, and maintenance is what fills it. A
block is allocated by taking the **least worn** free block, which spreads wear
without any bookkeeping: writing to it makes it no longer the least worn, so
the next write goes elsewhere.

Two blocks are reserved beyond what volumes can claim — the two copies of the
volume table — plus one spare so that a commit always has somewhere to go.

## Maintenance

There is no background thread. Nothing moves unless the application calls
`ubi_maintenance()`, which takes a budget in units of work and reports what it
did and what is left.

**`UBI_MAINTENANCE_RECLAIM`** erases blocks whose contents are dead and stamps
them with a fresh erase counter. This is where the cost of an erase is paid.
Keeping it topped up is what makes a write cost no erase at all.

**`UBI_MAINTENANCE_RELOCATE`** is wear levelling. When the gap between the most
worn block and some block in use exceeds `CONFIG_UBI_WEAR_LEVELING_THRESHOLD`,
the contents of the least worn such block are copied onto the most worn free
one, and the block with more life left goes back into rotation. The data's
checksum is verified on the way, never recomputed — moving a block must not
launder damage into a fresh seal.

A block that was just handed out is left alone for
`CONFIG_UBI_PROTECTION_CYCLES` erases. It is the least worn block in use the
moment anything lands on it, so levelling would reach for it first, and moving
data the caller has only just written — and may be about to replace — is wasted
work. Without this guard, a pool of worn blocks suddenly flooded with barely
used ones (say, after a cold volume is removed) will relocate *every* write
straight back out, doubling the erase count.

**`UBI_MAINTENANCE_REPAIR`** brings the two copies of the volume table back
into agreement, and gives retired blocks a second chance. A block that fails
that chance is written off rather than reported as an error: a part wearing out
is something this device is expected to live through, not a failure of the
repair. It stops being counted as work waiting, and stays in `bad_pebs`.

## Trust

Two callbacks, both required. Ignoring what UBI finds has to be something the
application writes down, not something it inherits from a zeroed field.

The **event callback** reports damage and inconsistency as they are found:
a header that failed its CRC, a header whose CRC passed but whose tag did not,
a lost copy of the volume table, an orphaned block, a retired block.

The **state callback** is a trust check. It is consulted at the end of every
attach and again every `CONFIG_UBI_STATE_CHECK_INTERVAL` metadata writes, so a
long uptime is not a way around it. It is asked *before* the write it guards,
which is what lets a refusal be honoured with nothing on the flash. This is
where rollback detection belongs — see [security.md](security.md).

## Relation to Linux UBI

The on-flash headers are Linux's, field for field, with a tag placed in space
Linux reserves as padding. An erase counter header written here passes
validation in unmodified Linux UBI.

What is kept:

- two passes at attach, and the mapping rebuilt in RAM rather than stored
- sequence numbers deciding duplicates, with the copy flag overriding them
- `copy_flag` left clear on an append, so a later write cannot invalidate a
  seal written before it
- wear levelling driven by the gap between the least worn block in use and the
  most worn free one
- a freshly handed out block protected from being moved for a while
- retirement held in RAM, never written to the flash

What is deliberately not ported:

- static volumes, and with them `used_ebs` and `data_pad`
- fastmap
- a background thread: maintenance here is explicit and budgeted
- scrubbing as a separate operation

What Linux does not have:

- authenticated metadata, and the events that fall out of it
- a trust callback, and with it a place for rollback detection

Two constants deserve a note. Linux defaults its wear levelling threshold to
4096 and protects a fresh block for 10 erases. This library uses 256 and 64.
The threshold follows Linux's own advice for flash rated under ten thousand
cycles; the protection window has to be larger to match, because a tighter
threshold reaches for a block far more often.
