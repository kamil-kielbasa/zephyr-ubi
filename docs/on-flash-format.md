# On-flash format

Format version 1. Every header and the volume table record carry a version
byte; a build that meets one it does not know refuses the attach with
`-ENOTSUP` and leaves the flash alone.

All multi-byte fields are big-endian.

## The physical erase block

```
offset 0     erase counter header    64 B
offset 64    volume identifier hdr   64 B
offset 128   data                    to the end of the block
```

Attach reads both headers in one 128-byte read. The write block has to divide
64, so each header is a whole number of write blocks.

Before a block is erased, the first four bytes of each header that still
verifies are zeroed, padded to a whole write block, the erase counter header
first (`CONFIG_UBI_ERASE_INVALIDATES_HEADERS`).

## Erase counter header

Written once, right after the erase.

| offset | size | field | in Linux |
|---|---|---|---|
| `0x00` | 4 | `magic` = `0x55424923` (`UBI#`) | same |
| `0x04` | 1 | `version` | same |
| `0x05` | 3 | padding | same |
| `0x08` | 8 | `ec`, the erase count | same |
| `0x10` | 4 | `vid_hdr_offset` = 64 | same |
| `0x14` | 4 | `data_offset` = 128 | same |
| `0x18` | 4 | `image_seq` | same |
| `0x1C` | 16 | **`MAC`**, AES-CMAC-128 | `padding2[0..15]` |
| `0x2C` | 16 | padding | `padding2[16..31]` |
| `0x3C` | 4 | `hdr_crc`, CRC32 over `0x00..0x3B` | same |

The MAC sits in Linux's padding, so this header passes validation in
unmodified Linux UBI.

## Volume identifier header

Written when a logical block is mapped onto the physical one.

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
| `0x18` | 16 | **`MAC`**, AES-CMAC-128 | `used_ebs`, `data_pad`, `data_crc`, `padding2` |
| `0x28` | 8 | `sqnum` | same |
| `0x30` | 4 | `data_crc` | `padding3[0..3]` |
| `0x34` | 8 | padding | `padding3[4..11]` |
| `0x3C` | 4 | `hdr_crc`, CRC32 over `0x00..0x3B` | same |

There are no static volumes, so `used_ebs` and `data_pad` are gone, and
`data_crc` moves to free 16 contiguous bytes for the MAC.

With `copy_flag` set, `data_crc` covers the first `data_size` bytes of data.
`ubi_leb_change()` sets it; `ubi_leb_write_at()` does not, since a later
append would break the checksum.

## MAC and CRC

The MAC covers the header except itself and the CRC, with the physical block
number in front:

```
message = be32(pnum) || header[0x00 .. mac_offset)
                     || header[mac_offset + 16 .. 0x3C)
```

`pnum` binds the header to where it was written, so a block copied elsewhere
no longer verifies.

```
writing                              reading
1. fill the fields                   1. magic     -> not a UBI block
2. AES-CMAC    -> MAC                2. CRC32     -> UBI_EVENT_HDR_CORRUPT
3. CRC32       -> hdr_crc            3. AES-CMAC  -> UBI_EVENT_HDR_TAMPERED
                                     4. use the fields
```

The CRC covers the MAC, so a torn header fails the CRC and a changed field
with a recomputed CRC fails the MAC.

## Volume table

Two copies, in blocks naming the reserved volume `0xFFFFFFFE`, each a sealed
record in the data area. The record:

| offset | size | field |
|---|---|---|
| `0x00` | 4 | `magic` = `0x55424956` (`UBIV`) |
| `0x04` | 1 | `version` = 1 |
| `0x05` | 3 | padding, zero |
| `0x08` | 4 | `revision`, one more for every layout change |
| `0x0C` | 4 | `image_seq` |
| `0x10` | 4 | `peb_size` |
| `0x14` | 4 | `peb_count` |
| `0x18` | 4 | `vol_id_watermark`; identifiers are never reused |
| `0x1C` | 4 | `volume_count` |

Then `volume_count` entries:

| offset | size | field |
|---|---|---|
| `0x00` | 4 | `vol_id` |
| `0x04` | 4 | `leb_count` |
| `0x08` | 16 | `name`, NUL-padded; a 16-character name fills it |

Then a 16-byte AES-CMAC over everything before it, under a key of its own.

A copy is used when its header has `copy_flag` set, the data checksum holds,
the record verifies, and its `image_seq` matches the header's. The record ends
where its `volume_count` says; `data_size` may run past it, since relocation
seals up to the last written write block.

## Keys

```
ikm_key_id  (the application's PSA key handle, never read)
  |
  +- HKDF-SHA256, salt "zephyr-ubi/v1",
  |  info "zephyr-ubi/header/v1" || key_context
  |    -> 128-bit key for both headers
  |
  +- HKDF-SHA256, salt "zephyr-ubi/v1",
     info "zephyr-ubi/volume-table/v1" || key_context
       -> 128-bit key for the volume table record
```

The key must carry `PSA_KEY_USAGE_DERIVE` and permit
`PSA_ALG_HKDF(PSA_ALG_SHA_256)`, or attach fails with `-EACCES`.
`key_context` is up to 32 bytes, appended as given; a partition attaches only
under the context it was formatted with.

The salt is a constant: `image_seq` lives in a header that cannot be verified
without the key. It is compared explicitly and sits inside the MAC message
instead.

The derived keys are volatile, cannot be exported, and are destroyed by
`ubi_device_deinit()`.
