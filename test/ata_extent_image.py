#!/usr/bin/env python3
"""Create and verify the isolated disk used by ata_extent.c."""
import argparse
from pathlib import Path
import struct

SIZE = 8 * 1024 * 1024
START = 2048
COUNT = 14329
SECTOR = 512


def create(path):
    with path.open("wb") as image:
        image.truncate(SIZE)
        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0" * 3, 0x83,
                                  b"\0" * 3, START, COUNT)
        mbr[510:512] = b"\x55\xaa"
        image.write(mbr)
        image.seek(START * SECTOR)
        image.write(bytes((i * 17 ^ (i >> 12)) & 255 for i in range(128 * SECTOR)))
        image.seek((START + COUNT - 1) * SECTOR)
        image.write(b"\x3c" * SECTOR)
        image.seek((START + COUNT) * SECTOR)
        image.write(b"\xa5" * (SIZE - (START + COUNT) * SECTOR))


def verify(path):
    with path.open("rb") as image:
        image.seek(START * SECTOR)
        actual = image.read(128 * SECTOR)
        expected = bytearray((i * 17 ^ (i >> 12)) & 255 for i in range(len(actual)))
        expected[63 * SECTOR:65 * SECTOR] = b"\x77" * (2 * SECTOR)
        assert actual == expected, "Extent write-back modified adjacent sectors"
        image.seek((START + COUNT - 1) * SECTOR)
        assert image.read(SECTOR) == b"\x5a" * SECTOR, "Final sector write-back failed"
        assert image.read() == b"\xa5" * (SIZE - (START + COUNT) * SECTOR), \
            "Write-back exceeded the partition boundary"
    print("PASS ATA extent write-back and partition guard")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("create", "verify"))
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    (create if args.operation == "create" else verify)(args.image)
