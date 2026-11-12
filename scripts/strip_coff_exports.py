#!/usr/bin/env python3
"""Remove DLL export directives from a COFF static archive without changing its index."""

import argparse
import mmap
import re
import struct
from pathlib import Path


EXPORT = re.compile(rb'(?i)(?<!\S)[/-]export:(?:"[^"]*"|[^\s"]+)(?:,[^\s]*)?')
MACHINES = {0x14C, 0x8664, 0xAA64}
BIGOBJ_CLASS_ID = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")


def export_ranges(data):
    if data[:8] != b"!<arch>\n":
        raise ValueError("Expected a COFF static archive")
    changes = []
    offset = 8
    while offset < len(data):
        header = data[offset : offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError("Invalid archive member header")
        size = int(header[48:58])
        start = offset + 60
        end = start + size
        if end > len(data):
            raise ValueError("Truncated archive member")
        name = header[:16].strip()
        count = 0
        table = end
        if size >= 20 and name not in (b"/", b"//", b"/SYM64/"):
            machine = struct.unpack_from("<H", data, start)[0]
            if machine in MACHINES:
                count = struct.unpack_from("<H", data, start + 2)[0]
                optional_size = struct.unpack_from("<H", data, start + 16)[0]
                table = start + 20 + optional_size
            elif (
                size >= 56
                and data[start : start + 4] == b"\0\0\xff\xff"
                and struct.unpack_from("<H", data, start + 4)[0] >= 2
                and data[start + 12 : start + 28] == BIGOBJ_CLASS_ID
                and struct.unpack_from("<H", data, start + 6)[0] in MACHINES
            ):
                count = struct.unpack_from("<I", data, start + 44)[0]
                table = start + 56
        if table + count * 40 > end:
            raise ValueError("Truncated COFF section table")
        for index in range(count):
            section = table + index * 40
            if data[section : section + 8].rstrip(b"\0") != b".drectve":
                continue
            raw_size, raw_offset = struct.unpack_from("<II", data, section + 16)
            if not raw_size:
                continue
            directives_start = start + raw_offset
            if directives_start < table + count * 40 or directives_start + raw_size > end:
                raise ValueError("Invalid COFF directive section")
            directives = data[directives_start : directives_start + raw_size]
            changes.extend(
                (directives_start + match.start(), directives_start + match.end())
                for match in EXPORT.finditer(directives)
            )
        offset = end + (size & 1)
    if offset != len(data):
        raise ValueError("Truncated archive padding")
    return changes


def strip_exports(path):
    with Path(path).open("r+b") as file:
        with mmap.mmap(file.fileno(), 0) as data:
            changes = export_ranges(data)
            for first, last in changes:
                data[first:last] = b" " * (last - first)
            data.flush()
    return len(changes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    arguments = parser.parse_args()
    removed = strip_exports(arguments.archive)
    print(f"Removed {removed} DLL export directives from {arguments.archive}")


if __name__ == "__main__":
    main()
