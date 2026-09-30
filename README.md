# UBI for Zephyr

[![build and test](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/ci.yml)
[![docs](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/docs.yml/badge.svg?branch=main)](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/docs.yml)

[![Docs](https://img.shields.io/badge/docs-GitHub%20Pages-blue)](https://kamil-kielbasa.github.io/zephyr-ubi/)
[![Zephyr](https://img.shields.io/badge/Zephyr-4.4-blueviolet)](https://zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A volume manager for raw flash, built as a Zephyr module. It brings the design
of Linux UBI to microcontrollers: one flash partition divided into named
volumes, wear spread across it, block updates that survive power loss, and its
own metadata authenticated with AES-CMAC.

## Features

- **Volumes** — named volumes of logical blocks, created, resized and removed
  at run time.
- **Power-loss safe** — a block replaced whole holds the old contents or the
  new ones after a power cut; appends cost no erase.
- **Wear levelling on demand** — erasing and relocation run when the
  application asks, on a budget it chooses; no background thread.
- **Authenticated metadata** — headers and the volume table carry an AES-CMAC
  under keys derived from a PSA key handle; damage and tampering are reported
  apart.
- **Rollback checks** — a state callback where the application compares the
  device with a store of its own, at attach and during use.
- **Linux-compatible headers** — laid out as in Linux UBI, with the MAC in
  space Linux leaves as padding.
- **Predictable footprint** — a fixed-size handle, a scratch buffer and 8
  bytes per erase block of heap, under 2 KiB of stack for any call.

Application data is stored as given: neither encrypted nor authenticated.

## Requirements

- Zephyr 4.4.
- PSA Crypto with AES-CMAC, HKDF and SHA-256: mbedTLS, TF-M or the platform's
  own.
- A fixed partition on flash with:
  - whole erase blocks of one size, 4 to 65534 of them, each larger than the
    two 64-byte headers and the volume table (320 bytes with 4 volumes);
  - a write block that divides 64 bytes;
  - every write block programmable once after an erase, in any order.

NOR flash, external or on-chip, meets these. On flash with ECC, turn off
`CONFIG_UBI_ERASE_INVALIDATES_HEADERS`: it writes over programmed bytes.

NAND is not supported. Its pages are larger than the 64-byte headers, must be
written in order, and need bad-block marking and ECC, none of which the Zephyr
flash API provides.

## Quick start

Add the module to the west manifest:

```yaml
    - name: zephyr-ubi
      url: https://github.com/kamil-kielbasa/zephyr-ubi
      revision: main
      path: modules/lib/zephyr-ubi
```

Build and run the sample, which formats a blank partition and counts boots:

```sh
west build -b native_sim modules/lib/zephyr-ubi/samples/basic -t run
```

## Documentation

Full documentation: <https://kamil-kielbasa.github.io/zephyr-ubi/>.

| Document | What you will find |
|---|---|
| [How it works](https://kamil-kielbasa.github.io/zephyr-ubi/how-it-works) | Blocks and their states, attach, writing, wear levelling, maintenance |
| [Security](https://kamil-kielbasa.github.io/zephyr-ubi/security) | Threat model, rollback detection, what protecting data is left to you |
| [Examples](https://kamil-kielbasa.github.io/zephyr-ubi/examples) | Setup, attaching, volumes, appends, key provisioning, maintenance |
| [Operations](https://kamil-kielbasa.github.io/zephyr-ubi/operations) | Errors, events, power loss, maintenance, key changes, resources |
| [On-flash format](https://kamil-kielbasa.github.io/zephyr-ubi/on-flash-format) | Headers, the volume table and key derivation, byte by byte |
| [API](include/ubi/ubi.h) | The contract of every call |

## License

MIT. See [LICENSE](LICENSE).

## Contact

email: kamkie1996@gmail.com
