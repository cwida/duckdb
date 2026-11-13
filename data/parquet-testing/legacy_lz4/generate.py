"""Generate tiny legacy LZ4 Parquet fixtures using only Python's standard library.

Each file has eight INT64 values (0..7), one row group, and one data
page. The compact Thrift encoding follows third_party/parquet/parquet.thrift.
Literal-only LZ4 blocks make the framing and malformed lengths reproducible.
Run this script to regenerate all fixtures in its directory.
"""

from pathlib import Path
import struct


# Thrift compact protocol type identifiers.
I32, I64, BINARY, LIST, STRUCT = 5, 6, 8, 9, 12


def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def integer(value):
    return varint((value << 1) ^ (value >> 63))


def binary(value):
    value = value.encode()
    return varint(len(value)) + value


def sequence(kind, values):
    size = len(values)
    header = bytes([(min(size, 15) << 4) | kind])
    if size >= 15:
        header += varint(size)
    return header + b"".join(values)


def record(fields):
    result = bytearray()
    previous = 0
    for field, kind, value in fields:
        result.append(((field - previous) << 4) | kind)
        result.extend(value)
        previous = field
    result.append(0)
    return bytes(result)


def literal_block(data):
    extra = len(data) - 15
    prefix = bytes([min(len(data), 15) << 4])
    if extra >= 0:
        prefix += b"\xff" * (extra // 255) + bytes([extra % 255])
    return prefix + data


def framed_block(chunks):
    result = struct.pack(">I", sum(map(len, chunks)))
    for chunk in chunks:
        compressed = literal_block(chunk)
        result += struct.pack(">I", len(compressed)) + compressed
    return result


def parquet(payload, version, levels=b""):
    # PageType DATA_PAGE = 0, DATA_PAGE_V2 = 3. Encoding PLAIN = 0, RLE = 3.
    if version == 1:
        data_header = record([(1, I32, integer(8)), (2, I32, integer(0)), (3, I32, integer(3)), (4, I32, integer(3))])
        page_kind, header_field = 0, 5
    else:
        data_header = record(
            [
                (1, I32, integer(8)),
                (2, I32, integer(0)),
                (3, I32, integer(8)),
                (4, I32, integer(0)),
                (5, I32, integer(len(levels))),
                (6, I32, integer(0)),
                (7, 1, b""),  # compact-protocol boolean true
            ]
        )
        page_kind, header_field = 3, 8
        # Data page v2 keeps definition levels outside the compressed payload.
        payload = levels + payload
    level_size = len(levels)
    if levels and version == 1:
        level_size += 4
    uncompressed_size = 64 + level_size
    header = record(
        [
            (1, I32, integer(page_kind)),
            (2, I32, integer(uncompressed_size)),
            (3, I32, integer(len(payload))),
            (header_field, STRUCT, data_header),
        ]
    )
    # Type INT64 = 2, FieldRepetitionType REQUIRED/OPTIONAL = 0/1, CompressionCodec LZ4 = 5.
    schema = [
        record([(4, BINARY, binary("schema")), (5, I32, integer(1))]),
        record([(1, I32, integer(2)), (3, I32, integer(1 if levels else 0)), (4, BINARY, binary("i"))]),
    ]
    metadata = record(
        [
            (1, I32, integer(2)),
            (2, LIST, sequence(I32, [integer(0), integer(3)])),
            (3, LIST, sequence(BINARY, [binary("i")])),
            (4, I32, integer(5)),
            (5, I64, integer(8)),
            (6, I64, integer(len(header) + uncompressed_size)),
            (7, I64, integer(len(header) + len(payload))),
            (9, I64, integer(4)),
        ]
    )
    column = record([(2, I64, integer(4)), (3, STRUCT, metadata)])
    row_group = record(
        [
            (1, LIST, sequence(STRUCT, [column])),
            (2, I64, integer(len(header) + uncompressed_size)),
            (3, I64, integer(8)),
        ]
    )
    footer = record(
        [
            (1, I32, integer(1)),
            (2, LIST, sequence(STRUCT, schema)),
            (3, I64, integer(8)),
            (4, LIST, sequence(STRUCT, [row_group])),
        ]
    )
    return b"PAR1" + header + payload + footer + struct.pack("<I", len(footer)) + b"PAR1"


def main():
    data = struct.pack("<8q", *range(8))
    compressed = literal_block(data)
    first_block = framed_block([data[:32]])
    single_block = framed_block([data])
    cases = {
        "single_block": single_block,
        "multi_block": first_block + framed_block([data[32:]]),
        "multi_chunk": framed_block([data[:16], data[16:40], data[40:]]),
        "terminator": single_block + struct.pack(">I", 0),
        "raw": compressed,
        "short_header": b"\0\0\0",
        "short_chunk_header": struct.pack(">I", 64) + b"\0\0",
        "oversize_block": struct.pack(">I", 0xFFFFFFFF) + single_block[4:],
        "oversize_chunk": struct.pack(">II", 64, 0xFFFFFFFF) + compressed,
        "zero_chunk": struct.pack(">II", 64, 0),
        "corrupt_chunk": struct.pack(">II", 64, 1) + b"\0",
        "short_output": struct.pack(">I", 63) + single_block[4:],
        "trailing_bytes": single_block + b"x",
        "missing_block": first_block,
        "zero_block": struct.pack(">I", 0),
        "truncated_second_block": first_block + framed_block([data[32:]])[:-1],
    }
    directory = Path(__file__).resolve().parent
    for name, payload in cases.items():
        for version in [1, 2]:
            (directory / f"{name}_v{version}.parquet").write_bytes(parquet(payload, version))
    # RLE definition levels: eight non-NULL values. V1 compresses the length and
    # levels with the data; V2 decompresses into the buffer after these levels.
    levels = b"\x10\x01"
    for version in [1, 2]:
        values = struct.pack("<I", len(levels)) + levels + data if version == 1 else data
        (directory / f"optional_levels_v{version}.parquet").write_bytes(
            parquet(framed_block([values]), version, levels)
        )


if __name__ == "__main__":
    main()
