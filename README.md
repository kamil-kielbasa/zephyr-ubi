# UBI for Zephyr

[![CI / Zephyr](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/ci.yml)
[![CI / Documentation](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/docs.yml/badge.svg?branch=main)](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/docs.yml)
[![codecov](https://codecov.io/gh/kamil-kielbasa/zephyr-ubi/branch/main/graph/badge.svg)](https://codecov.io/gh/kamil-kielbasa/zephyr-ubi)

[![Docs](https://img.shields.io/badge/docs-GitHub%20Pages-blue)](https://kamil-kielbasa.github.io/zephyr-ubi/)
[![Release](https://img.shields.io/github/v/release/kamil-kielbasa/zephyr-ubi)](https://github.com/kamil-kielbasa/zephyr-ubi/releases)
[![Zephyr](https://img.shields.io/badge/Zephyr-4.4-blueviolet)](https://zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A volume manager for raw flash, built as a Zephyr module. It brings the design
of Linux UBI to microcontrollers: one flash partition divided into named
volumes, wear spread across it, block updates that survive power loss, and its
own metadata authenticated with AES-CMAC.

## Features

| | UBI for Zephyr | Linux UBI |
|---|:---:|:---:|
| Named volumes, created, resized and removed at run time | ✓ | ✓ |
| Volume rename | ✗ | ✓ |
| Static volumes | ✗ | ✓ |
| Atomic block replace, `ubi_leb_change()` | ✓ | ✓ |
| Appends with no erase, `ubi_leb_write_at()` | ✓ | ✓ |
| Wear levelling and erasing of released blocks | on demand | background thread |
| Scrubbing after bit flips | ✗ | ✓ |
| Fastmap | ✗ | ✓ |
| Headers and volume table authenticated with AES-CMAC | ✓ | ✗ |
| Damage and tampering reported apart | ✓ | ✗ |
| Rollback check through a state callback | ✓ | ✗ |
| Block header re-authenticated on every read | option | ✗ |

On demand means `ubi_maintenance()`, on a budget the application chooses; a
write that finds no free block erases one itself. Headers are laid out as in
Linux UBI, with the MAC in space Linux leaves as padding. Application data is
stored as given: neither encrypted nor authenticated.

## Requirements

| Component | Requirement |
|---|---|
| Zephyr | 4.4 |
| Cryptography | PSA Crypto with AES-CMAC, HKDF and SHA-256 (mbedTLS, TF-M or the platform's own) |
| Partition | one fixed partition of 4 to 65534 erase blocks of one size |
| Erase block | larger than the two 64-byte headers and the volume table (320 bytes with 4 volumes) |
| Write block | divides 64 bytes |
| Programming | every write block once after an erase, in any order |
| RAM | a fixed-size handle; on the heap, a scratch buffer and 8 bytes per erase block |
| Stack | under 2 KiB for any call |

Exact figures: [Resources](https://kamil-kielbasa.github.io/zephyr-ubi/operations#resources)
and [Limits](https://kamil-kielbasa.github.io/zephyr-ubi/operations#limits).

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
