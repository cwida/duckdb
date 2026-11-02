# Generates the decimal fixtures with precision > 38, used by
# test/sql/copy/parquet/test_parquet_decimal_gt38.test.
# DuckDB reads these columns as DOUBLE (its DECIMAL type is capped at width 38).
import decimal
import struct

import pyarrow as pa
import pyarrow.parquet as pq

D = decimal.Decimal

# decimal(39,2): pyarrow stores it as a 17-byte FIXED_LEN_BYTE_ARRAY, so the
# big-endian two's complement value does not divide evenly into 8-byte chunks.
# -0.01 has unscaled magnitude 1, the smallest negative; -0.02 the next one up.
pq.write_table(
    pa.table({"x": pa.array([D("2.25"), D("-2.25"), D("-0.01"), D("-0.02"), D("1.00"), D("-1.00")],
                            type=pa.decimal256(39, 2))}),
    "fixed_length_decimal_gt38.parquet",
)

# decimal(50,2): 21-byte FIXED_LEN_BYTE_ARRAY, a different trailing-chunk width.
pq.write_table(
    pa.table({"x": pa.array([D("1.50"), D("-2.25"), D("123456789.10")], type=pa.decimal256(50, 2))}),
    "fixed_length_decimal_gt38_len21.parquet",
)

# decimal(39,2) split into two row groups (negatives, then positives), each with
# min/max statistics, so zone-map pruning of filters can be exercised.
vals = [D(f"-{i}.00") for i in range(1, 6)] + [D(f"{i}.00") for i in range(1, 6)]
pq.write_table(
    pa.table({"x": pa.array(vals, type=pa.decimal256(39, 2))}),
    "fixed_length_decimal_gt38_row_groups.parquet",
    row_group_size=5,
)


# decimal(39,2) stored as variable-length BYTE_ARRAY, where every value carries its
# own byte length. pyarrow only writes FIXED_LEN_BYTE_ARRAY decimals, so this file
# is assembled by hand: PLAIN encoding, uncompressed, two row groups with statistics.
def uvarint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])


def zz(n):
    return uvarint((n << 1) ^ (n >> 63))


# thrift compact-protocol field types
I32, I64, BIN, LST, STR = 5, 6, 8, 9, 12


def struct_(fields):
    out = b""
    last = 0
    for fid, ct, payload in fields:
        delta = fid - last
        assert 1 <= delta <= 15
        out += bytes([(delta << 4) | ct]) + payload
        last = fid
    return out + b"\x00"


def binary(b):
    return uvarint(len(b)) + b


def list_(elem_ct, elems):
    n = len(elems)
    hdr = bytes([(n << 4) | elem_ct]) if n < 15 else bytes([0xF0 | elem_ct]) + uvarint(n)
    return hdr + b"".join(elems)


def enc_unscaled(v):
    # minimal big-endian two's complement
    n = max(1, (v.bit_length() + 8) // 8)
    return v.to_bytes(n, "big", signed=True)


# unscaled values (scale=2): RG0 = [300, 70000] -> 3.00, 700.00 ; RG1 = [1] -> 0.01
row_groups = [[300, 70000], [1]]

out = b"PAR1"
rg_meta = []
for vals in row_groups:
    data = b"".join(struct.pack("<I", len(enc_unscaled(v))) + enc_unscaled(v) for v in vals)
    page_hdr = struct_([
        (1, I32, zz(0)),                 # PageType DATA_PAGE
        (2, I32, zz(len(data))),         # uncompressed_page_size
        (3, I32, zz(len(data))),         # compressed_page_size
        (5, STR, struct_([               # DataPageHeader
            (1, I32, zz(len(vals))),
            (2, I32, zz(0)),             # PLAIN
            (3, I32, zz(3)),             # RLE def levels (none written: required col)
            (4, I32, zz(3)),
        ])),
    ])
    offset = len(out)
    out += page_hdr + data
    mn, mx = enc_unscaled(min(vals)), enc_unscaled(max(vals))
    stats = struct_([
        (1, BIN, binary(mx)),            # max (deprecated)
        (2, BIN, binary(mn)),            # min (deprecated)
        (3, I64, zz(0)),                 # null_count
        (5, BIN, binary(mx)),            # max_value
        (6, BIN, binary(mn)),            # min_value
    ])
    total = len(page_hdr) + len(data)
    col_meta = struct_([
        (1, I32, zz(6)),                 # Type BYTE_ARRAY
        (2, LST, list_(I32, [zz(0), zz(3)])),  # encodings PLAIN, RLE
        (3, LST, list_(BIN, [binary(b"x")])),
        (4, I32, zz(0)),                 # UNCOMPRESSED
        (5, I64, zz(len(vals))),
        (6, I64, zz(total)),
        (7, I64, zz(total)),
        (9, I64, zz(offset)),            # data_page_offset
        (12, STR, stats),
    ])
    chunk = struct_([(2, I64, zz(offset)), (3, STR, col_meta)])
    rg_meta.append(struct_([
        (1, LST, list_(STR, [chunk])),
        (2, I64, zz(total)),
        (3, I64, zz(len(vals))),
    ]))

root = struct_([(4, BIN, binary(b"schema")), (5, I32, zz(1))])
col = struct_([
    (1, I32, zz(6)),   # BYTE_ARRAY
    (3, I32, zz(0)),   # REQUIRED
    (4, BIN, binary(b"x")),
    (6, I32, zz(5)),   # ConvertedType DECIMAL
    (7, I32, zz(2)),   # scale
    (8, I32, zz(39)),  # precision
])
footer = struct_([
    (1, I32, zz(1)),
    (2, LST, list_(STR, [root, col])),
    (3, I64, zz(sum(len(v) for v in row_groups))),
    (4, LST, list_(STR, rg_meta)),
    (6, BIN, binary(b"handmade")),
])
out += footer + struct.pack("<I", len(footer)) + b"PAR1"

with open("byte_array_decimal_gt38.parquet", "wb") as f:
    f.write(out)
