# On-flash format

Everything here is a contract. A build that changes any of it cannot read a
device written by a build that did not.

All multi-byte fields are big-endian, as in Linux UBI.

## The physical erase block

```
offset 0     erase counter header    64 B
offset 64    volume identifier hdr   64 B
offset 128   data                    to the end of the block
```

The two headers are adjacent so that attach fetches both in one 128-byte read.
This assumes NOR flash, where a page may be programmed in several passes. NAND
is out of scope.

## Erase counter header

Written once, when the block is erased, and never touched again until the next
erase.

| offset | size | field | in Linux |
|---|---|---|---|
| `0x00` | 4 | `magic` = `0x55424923` (`UBI#`) | same |
| `0x04` | 1 | `version` | same |
| `0x05` | 3 | padding | same |
| `0x08` | 8 | `ec` — erase count | same |
| `0x10` | 4 | `vid_hdr_offset` = 64 | same |
| `0x14` | 4 | `data_offset` = 128 | same |
| `0x18` | 4 | `image_seq` | same |
| `0x1C` | 16 | **`tag`** — AES-CMAC-128 | `padding2[0..15]` |
| `0x2C` | 16 | padding | `padding2[16..31]` |
| `0x3C` | 4 | `hdr_crc` — CRC32 over `0x00..0x3B` | same |

Every field Linux interprets sits at the same offset and means the same thing.
The tag occupies space Linux reserves as padding, and the CRC is computed the
same way — over everything before it, so over the tag too. **An erase counter
header written here passes validation in unmodified Linux UBI.**

## Volume identifier header

Written when a logical block is mapped onto this physical one.

| offset | size | field | in Linux |
|---|---|---|---|
| `0x00` | 4 | `magic` = `0x55424921` (`UBI!`) | same |
| `0x04` | 1 | `version` | same |
| `0x05` | 1 | `vol_type` = dynamic | same |
| `0x06` | 1 | `copy_flag` | same |
| `0x07` | 1 | `compat` = 0 | same |
| `0x08` | 4 | `vol_id` | same |
| `0x0C` | 4 | `lnum` | same |
| `0x10` | 4 | `image_seq` | `padding1` |
| `0x14` | 4 | `data_size` | same |
| `0x18` | 16 | **`tag`** — AES-CMAC-128 | `used_ebs`, `data_pad`, `data_crc`, `padding2` |
| `0x28` | 8 | `sqnum` | same |
| `0x30` | 4 | `data_crc` | `padding3[0..3]` |
| `0x34` | 8 | padding | `padding3[4..11]` |
| `0x3C` | 4 | `hdr_crc` — CRC32 over `0x00..0x3B` | same |

Three fields diverge from Linux. `used_ebs` and `data_pad` are gone because
there are no static volumes here, and `data_crc` moves into what Linux uses as
`padding3` to free sixteen contiguous bytes for the tag.

`copy_flag` and `data_size` together are a promise: when the flag is set, the
header names how many bytes of data follow and `data_crc` covers exactly those
bytes. `ubi_leb_change()` sets it. `ubi_leb_write_at()` does not, because a
later append would invalidate a checksum written before it — the same reason
Linux leaves the flag clear for `ubi_leb_write()`.

## What the tag covers

The tag authenticates the whole header except itself and the CRC, with the
physical block number prepended:

```
message = be32(pnum) || header[0x00 .. tag_offset)
                     || header[tag_offset + 16 .. 0x3C)

erase counter header:  4 + 28 + 16 = 48 bytes
volume identifier hdr: 4 + 24 + 20 = 48 bytes
```

`pnum` costs nothing on the flash and binds the header to the position it was
written at, so a block copied elsewhere no longer verifies.

## The order of operations

```
writing                              reading
1. fill the fields                   1. magic     -> this is not a UBI device
2. CMAC        -> tag                2. CRC32     -> UBI_EVENT_HDR_CORRUPT
3. CRC32       -> hdr_crc            3. CMAC      -> UBI_EVENT_HDR_TAMPERED
                                     4. only now use the fields
```

Computing the CRC last means it covers the tag. That is what separates an
interrupted write from a forgery: a torn header fails the CRC, while a changed
field with a recomputed CRC fails the tag.

## The volume table

Two copies live in blocks claiming the reserved volume `0xFFFFFFFE`. The record
sits in the data area behind the headers and is written whole on every change,
newest sequence number wins.

Preamble, 32 bytes:

| offset | size | field |
|---|---|---|
| `0x00` | 4 | `magic` = `0x55424956` (`UBIV`) |
| `0x04` | 4 | `version` |
| `0x08` | 4 | `revision` — bumped on every layout change |
| `0x0C` | 4 | `image_seq` |
| `0x10` | 4 | `peb_size` |
| `0x14` | 4 | `peb_count` |
| `0x18` | 4 | `vol_id_watermark` — no volume id is ever reused |
| `0x1C` | 4 | `volume_count` |

Then `volume_count` entries of 24 bytes each:

| offset | size | field |
|---|---|---|
| `0x00` | 4 | `vol_id` |
| `0x04` | 4 | `leb_count` |
| `0x08` | 16 | `name`, NUL-terminated |

Then a 16-byte AES-CMAC tag over everything before it.

The record is sealed with a different key from the headers, and its length and
CRC32 live in the volume identifier header in front of it — which is itself
sealed. So a changed record fails a checksum whose author could not have
repaired it without the key. That is why a damaged copy and a forged one are
reported as one event, `UBI_EVENT_VOLUME_TABLE_CORRUPT`, rather than two.

## Keys

The application hands over a PSA key handle, never bytes. The key must carry
`PSA_KEY_USAGE_DERIVE` and permit `PSA_ALG_HKDF(PSA_ALG_SHA_256)`; without
either, attach fails with `-EACCES`.

```
ikm_key_id  (the application's handle, never read)
  |
  +- HKDF-SHA256, salt "zephyr-ubi/v1", info "zephyr-ubi/header/v1"
  |    -> 128-bit key for the erase counter and volume identifier headers
  |
  +- HKDF-SHA256, salt "zephyr-ubi/v1", info "zephyr-ubi/volume-table/v1"
       -> 128-bit key for the volume table record
```

The salt is a compile-time constant, not the image sequence number. Salting
with `image_seq` would be circular: the key would depend on a field living in a
header that cannot be verified without the key, and attach would have nothing
to start from. Nothing is lost by dropping it, because `image_seq` is compared
explicitly **and** sits inside the MAC message — a stale header fails the
comparison and a forged one fails the tag.

The derived keys are non-exportable, volatile, and destroyed by
`ubi_device_deinit()`. The handle the application supplied is never touched.
No raw key material crosses the API in either direction.
