#!/usr/bin/env python3
# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT
"""Known answers for the on-flash format, computed without the library.

Derives both keys from the keying material the unit tests import, seals one
erase counter header, one volume identifier header and one volume table
record exactly as docs/on-flash-format.md describes them, and prints the
results as C initialisers for tests/unit/src/test_kat.c.

Nothing here is shared with the C code, so a change to either one that
alters what reaches the flash shows up as a mismatch.

Needs the 'cryptography' package.
"""

import struct
import zlib

from cryptography.hazmat.primitives import cmac, hashes
from cryptography.hazmat.primitives.ciphers import algorithms
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

IKM = bytes(range(32))
SALT = b"zephyr-ubi/v1"
LABEL_HEADER = b"zephyr-ubi/header/v1"
LABEL_VOLUME_TABLE = b"zephyr-ubi/volume-table/v1"
KEY_CONTEXT = b"storage_partition"

HEADER_SIZE = 64
CRC_OFFSET = 0x3C
MAC_SIZE = 16

PNUM = 7
IMAGE_SEQ = 0x12345678


def derive(label, context=b""):
    return HKDF(algorithm=hashes.SHA256(), length=16, salt=SALT,
                info=label + context).derive(IKM)


def mac(key, message):
    c = cmac.CMAC(algorithms.AES(key))
    c.update(message)
    return c.finalize()


def seal(key, pnum, header, mac_offset):
    message = (struct.pack(">I", pnum) + bytes(header[:mac_offset]) +
               bytes(header[mac_offset + MAC_SIZE:CRC_OFFSET]))
    assert len(message) == 48
    header[mac_offset:mac_offset + MAC_SIZE] = mac(key, message)
    struct.pack_into(">I", header, CRC_OFFSET,
                     zlib.crc32(bytes(header[:CRC_OFFSET])))
    return bytes(header)


def ec_header(key):
    header = bytearray(HEADER_SIZE)
    struct.pack_into(">I", header, 0x00, 0x55424923)
    header[0x04] = 1
    struct.pack_into(">Q", header, 0x08, 1)
    struct.pack_into(">I", header, 0x10, 64)
    struct.pack_into(">I", header, 0x14, 128)
    struct.pack_into(">I", header, 0x18, IMAGE_SEQ)
    return seal(key, PNUM, header, 0x1C)


def vid_header(key):
    header = bytearray(HEADER_SIZE)
    struct.pack_into(">I", header, 0x00, 0x55424921)
    header[0x04] = 1
    header[0x05] = 1
    header[0x06] = 1
    struct.pack_into(">I", header, 0x08, 3)
    struct.pack_into(">I", header, 0x0C, 9)
    struct.pack_into(">I", header, 0x10, IMAGE_SEQ)
    struct.pack_into(">I", header, 0x14, 256)
    struct.pack_into(">Q", header, 0x28, 0x0102030405060708)
    struct.pack_into(">I", header, 0x30, 0xCAFEF00D)
    return seal(key, PNUM, header, 0x18)


def volume_table_record(key):
    volumes = [(0, 16, b"logs"), (2, 5, b"0123456789abcde")]
    record = bytearray(32 + 24 * len(volumes) + MAC_SIZE)
    struct.pack_into(">I", record, 0x00, 0x55424956)
    record[0x04] = 1
    struct.pack_into(">IIIIII", record, 0x08, 7, IMAGE_SEQ, 4096, 2048, 3,
                     len(volumes))
    for index, (vol_id, leb_count, name) in enumerate(volumes):
        at = 32 + 24 * index
        struct.pack_into(">II", record, at, vol_id, leb_count)
        record[at + 8:at + 8 + len(name)] = name
    record[-MAC_SIZE:] = mac(key, bytes(record[:-MAC_SIZE]))
    return bytes(record)


def c_array(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for at in range(0, len(data), 8):
        chunk = ", ".join(f"0x{byte:02X}" for byte in data[at:at + 8])
        lines.append(f"\t{chunk},")
    lines.append("};")
    return "\n".join(lines)


def main():
    key_header = derive(LABEL_HEADER)
    key_volume_table = derive(LABEL_VOLUME_TABLE)

    print(c_array("kat_header_key_fingerprint",
                  mac(key_header, b"fingerprint")))
    print()
    print(c_array("kat_volume_table_key_fingerprint",
                  mac(key_volume_table, b"fingerprint")))
    print()
    print(c_array("kat_ec_header", ec_header(key_header)))
    print()
    print(c_array("kat_vid_header", vid_header(key_header)))
    print()
    print(c_array("kat_volume_table_record",
                  volume_table_record(key_volume_table)))
    print()
    print(c_array("kat_bound_header_key_fingerprint",
                  mac(derive(LABEL_HEADER, KEY_CONTEXT), b"fingerprint")))
    print()
    print(c_array("kat_bound_volume_table_key_fingerprint",
                  mac(derive(LABEL_VOLUME_TABLE, KEY_CONTEXT),
                      b"fingerprint")))


if __name__ == "__main__":
    main()
