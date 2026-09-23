# Changelog

## v0.1

First release.

### Storage

- One flash partition is divided into named volumes, which can be created,
  grown, shrunk and removed at run time.
- A volume is read and written in logical blocks; where each one lives on the
  flash is UBI's business and changes over time.
- A whole block is replaced atomically: after a power cut it holds either its
  previous contents or the new ones.
- Records can be appended to a block at an offset the application chooses.
- A block can be erased on demand, taking its contents off the flash at once.

### Lifetime

- Wear is spread across the partition, so data that is rewritten often does
  not wear out one part of the flash.
- Erasing and wear levelling run only when the application asks, with a
  budget it chooses; there is no background thread.
- A block that fails a write or an erase is taken out of service and given
  another chance later.

### Integrity and security

- Every header and the volume table carry an AES-CMAC, so a forged or altered
  one is detected and reported instead of obeyed.
- Damage is told apart from tampering, damaged blocks are kept for
  inspection, and the device keeps working with one volume table copy lost.
- The counters needed for rollback detection are reported to the application,
  which decides whether to trust the device; a refusal leaves it read-only.
- Application data is not encrypted.

### Compatibility

- The on-flash headers are laid out as in Linux UBI.
- Tested on native_sim in four flash geometries and on the external flash of
  the nRF5340 DK.
