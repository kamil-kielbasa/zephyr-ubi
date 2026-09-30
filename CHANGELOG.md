# Changelog

## v0.1

First release.

### Storage

- One flash partition divided into named volumes, created, resized and removed
  at run time.
- Volumes are read and written in logical blocks; UBI decides where each one
  lives and moves it over time.
- `ubi_leb_change()` replaces a block atomically: after a power cut it holds
  the previous contents or the new ones.
- `ubi_leb_write_at()` appends at an offset the application chooses.
- `ubi_leb_erase()` takes a block and every older copy of it off the flash.

### Lifetime

- Wear is spread across the partition by allocation and by relocating rarely
  rewritten data.
- Erasing, wear levelling, repair and discarding damaged blocks run only when
  the application calls `ubi_maintenance()`, on a budget it chooses.
- A block that fails a write is retired and given another chance later; a
  failed erase makes the device read-only until the next attach.
- Header magics are cleared before an erase, so an erase cut short is
  recognised.

### Integrity and security

- Headers and the volume table carry an AES-CMAC under keys derived with
  HKDF-SHA256 from a PSA key handle; `key_context` separates partitions
  sharing keying material.
- Damage and tampering are reported apart through the event callback; blocks
  damaged behind a valid erase counter header are kept until discarded.
- Two copies of the volume table, each updated into a fresh block; the device
  carries on with one and reports it.
- A state callback, at attach and every `CONFIG_UBI_STATE_CHECK_INTERVAL`
  writes, decides whether to trust the device; a refusal leaves it read-only.
- Attach writes nothing, and only `-ENODEV` means a partition holds nothing
  to lose.
- Application data is not encrypted.

### Compatibility

- On-flash format version 1, with headers laid out as in Linux UBI.
- Zephyr 4.4 with PSA Crypto; tested on `native_sim` and on the nRF5340 DK's
  external flash.
