#!/usr/bin/env python3
"""Corrupts the page index of a Parquet file in place, for the robustness tests in test/sql/copy/parquet/page_index.

    corrupt_page_index.py offset_index <in> <out> <offset_index_offset> <page> <new_first_row_index>
        rewrites page_locations[page].first_row_index (the new value must encode to a varint of the same length)
    corrupt_page_index.py column_index_minlen <in> <out> <column_index_offset> <page>
        shortens min_values[page] by one byte (the struct shrinks by one byte, a padding byte keeps the layout)
    corrupt_page_index.py offset_index_swap <in> <out> <offset_index_offset> <page a> <page b>
        swaps the offsets of two page_locations, so the pages are listed out of byte order
    corrupt_page_index.py offset_index_copy <in> <out> <source column> <target column>
        copies the OffsetIndex bytes of one column chunk of the first row group over those of another chunk, so the
        target chunk's index points at the pages of the source chunk (the target's OffsetIndex has to be the last
        thing before the footer, which is where DuckDB writes the index of the last column)

The offsets come from parquet_metadata(): column_index_offset / offset_index_offset of the column chunk. The files
corrupt_*.parquet in this directory were made from corrupt_base.parquet (3000 rows, id BIGINT in pages of 512 rows):
    offset_index corrupt_base.parquet corrupt_first_row_index.parquet <offset_index_offset of id> 1 300
    offset_index corrupt_base.parquet corrupt_non_monotonic.parquet <offset_index_offset of id> 2 100
    column_index_minlen corrupt_base.parquet corrupt_min_length.parquet <column_index_offset of id> 0

corrupt_offset_index_other_column.parquet was made from a file with the same page row counts in both columns:
    COPY (SELECT r AS id, TIMESTAMP '2020-01-01' + INTERVAL (r) SECOND AS ts FROM range(3000) t(r))
        TO 'base.parquet' (FORMAT parquet, WRITE_PAGE_INDEX true, DATA_PAGE_SIZE_LIMIT 4096, PARQUET_VERSION V2);
    offset_index_copy base.parquet corrupt_offset_index_other_column.parquet 0 1
"""
import sys


def read_varint(data, pos):
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7f) << shift
        if not (byte & 0x80):
            return result, pos
        shift += 7


def write_varint(value):
    out = bytearray()
    while True:
        if value < 0x80:
            out.append(value)
            return bytes(out)
        out.append((value & 0x7f) | 0x80)
        value >>= 7


def zigzag_decode(value):
    return (value >> 1) ^ -(value & 1)


def zigzag_encode(value):
    return (value << 1) ^ (value >> 63)


def skip_field(data, pos, field_type):
    if field_type in (1, 2):
        return pos
    if field_type == 3:
        return pos + 1
    if field_type in (4, 5, 6):
        return read_varint(data, pos)[1]
    if field_type == 7:
        return pos + 8
    if field_type == 8:
        length, pos = read_varint(data, pos)
        return pos + length
    if field_type == 9:
        header = data[pos]
        pos += 1
        count = header >> 4
        element_type = header & 0xf
        if count == 15:
            count, pos = read_varint(data, pos)
        for _ in range(count):
            pos = skip_field(data, pos, element_type)
        return pos
    if field_type == 12:
        return read_struct(data, pos)[1]
    raise ValueError("unsupported thrift type %d" % field_type)


def read_struct(data, pos):
    """{field id: (type, position of the value)} and the position after the struct."""
    fields = {}
    last_id = 0
    while True:
        header = data[pos]
        pos += 1
        if header == 0:
            return fields, pos
        delta = header >> 4
        field_type = header & 0xf
        if delta == 0:
            field_id, pos = read_varint(data, pos)
            field_id = zigzag_decode(field_id)
        else:
            field_id = last_id + delta
        last_id = field_id
        fields[field_id] = (field_type, pos)
        pos = skip_field(data, pos, field_type)


def list_elements(data, pos):
    header = data[pos]
    pos += 1
    count = header >> 4
    element_type = header & 0xf
    if count == 15:
        count, pos = read_varint(data, pos)
    positions = []
    for _ in range(count):
        positions.append(pos)
        pos = skip_field(data, pos, element_type)
    return element_type, positions, pos


def column_chunks(data, footer_start):
    """The {field id: (type, position)} maps of the ColumnChunk structs of the first row group."""
    file_metadata, _ = read_struct(data, footer_start)
    _, row_groups, _ = list_elements(data, file_metadata[4][1])
    row_group, _ = read_struct(data, row_groups[0])
    _, chunks, _ = list_elements(data, row_group[1][1])
    return [read_struct(data, chunk)[0] for chunk in chunks]


def read_i64(data, pos):
    return zigzag_decode(read_varint(data, pos)[0])


def main():
    mode, source, target = sys.argv[1], sys.argv[2], sys.argv[3]
    data = bytearray(open(source, "rb").read())
    if mode == "offset_index":
        offset, page, new_value = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
        fields, _ = read_struct(data, offset)
        _, pages, _ = list_elements(data, fields[1][1])
        location_fields, _ = read_struct(data, pages[page])
        _, pos = location_fields[3]
        old, end = read_varint(data, pos)
        new = write_varint(zigzag_encode(new_value))
        assert len(new) == end - pos, "the new value must encode to %d bytes" % (end - pos)
        print("page %d first_row_index %d -> %d" % (page, zigzag_decode(old), new_value))
        data[pos:end] = new
    elif mode == "column_index_minlen":
        offset, page = int(sys.argv[4]), int(sys.argv[5])
        fields, end = read_struct(data, offset)
        _, mins, _ = list_elements(data, fields[2][1])
        pos = mins[page]
        length, value_pos = read_varint(data, pos)
        assert 1 < length < 0x80
        print("min_values[%d] length %d -> %d" % (page, length, length - 1))
        data[pos] = length - 1
        del data[value_pos + length - 1]
        data.insert(end - 1, 0)
    elif mode == "offset_index_swap":
        offset, page_a, page_b = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
        fields, _ = read_struct(data, offset)
        _, pages, _ = list_elements(data, fields[1][1])
        spans = []
        for page in (page_a, page_b):
            location_fields, _ = read_struct(data, pages[page])
            _, pos = location_fields[1]
            value, end = read_varint(data, pos)
            spans.append((pos, end, zigzag_decode(value)))
        (a_pos, a_end, a_value), (b_pos, b_end, b_value) = sorted(spans)
        print("page offsets %d <-> %d" % (a_value, b_value))
        # the two varints may differ in length; the OffsetIndex keeps its total size, so nothing after it moves
        data[a_pos:b_end] = data[b_pos:b_end] + data[a_end:b_pos] + data[a_pos:a_end]
    elif mode == "offset_index_copy":
        source_column, target_column = int(sys.argv[4]), int(sys.argv[5])
        footer_start = len(data) - 8 - int.from_bytes(data[-8:-4], "little")
        chunks = column_chunks(data, footer_start)
        source_offset = read_i64(data, chunks[source_column][4][1])
        source_length = read_i64(data, chunks[source_column][5][1])
        target_offset = read_i64(data, chunks[target_column][4][1])
        length_pos = chunks[target_column][5][1]
        target_length, length_end = read_varint(data, length_pos)
        target_length = zigzag_decode(target_length)
        assert target_offset + target_length == footer_start, "the target OffsetIndex must end where the footer starts"
        new_length = write_varint(zigzag_encode(source_length))
        assert len(new_length) == length_end - length_pos, "the new length must encode to %d bytes" % (
            length_end - length_pos)
        print("OffsetIndex of column %d (%d bytes at %d) copied over column %d (%d bytes at %d)"
              % (source_column, source_length, source_offset, target_column, target_length, target_offset))
        # the footer keeps its size, so patch it first and then replace the index bytes in front of it
        data[length_pos:length_end] = new_length
        data[target_offset:target_offset + target_length] = data[source_offset:source_offset + source_length]
    else:
        raise SystemExit("unknown mode " + mode)
    open(target, "wb").write(data)


if __name__ == "__main__":
    main()
