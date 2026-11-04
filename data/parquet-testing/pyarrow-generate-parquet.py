import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pyarrow.csv as csv
from pathlib import Path


def generate_parquet(data_dir: Path):
    generate_silly_names(data_dir / 'silly-names.parquet')
    generate_byte_stream_split(data_dir / 'byte_stream_split.parquet')
    generate_byte_stream_split_flba(data_dir / 'byte_stream_split_flba.parquet')
    generate_time_millis(data_dir / 'time-millis.parquet')


def generate_silly_names(path: Path):
    df = pd.DataFrame({'önë': [1, 2, 3],
                       '': ['foo', 'bar', 'baz'],
                       '🦆': [True, False, True]})
    table = pa.Table.from_pandas(df)
    pq.write_table(table, path)


def generate_time_millis(path: Path):
    times = pa.array([0, 45296789, 86399999, None], type=pa.time32('ms'))
    pq.write_table(pa.Table.from_arrays([times], ['t']), path)


def generate_byte_stream_split(path: Path):
    num_rows = 100
    rng = np.random.default_rng(0)

    floats = pa.array(rng.uniform(-100.0, 100.0, num_rows), type=pa.float32())
    doubles = pa.array(rng.uniform(-100.0, 100.0, num_rows), type=pa.float64())
    ints = pa.array(rng.integers(-2147483648, 2147483647, num_rows), type=pa.int32())
    longs = pa.array(rng.integers(-9223372036854775808, 9223372036854775807, num_rows), type=pa.int64())

    null_mask = np.ones(num_rows, dtype=np.bool_)
    null_mask[num_rows // 10:] = False
    rng.shuffle(null_mask)
    nullable_floats = pa.array(
            rng.uniform(-100.0, 100.0, num_rows), type=pa.float32(), mask=null_mask)

    table = pa.Table.from_arrays(
            [floats, doubles, ints, longs, nullable_floats],
            ["floats", "doubles", "ints", "longs", "nullable_floats"])

    with pq.ParquetWriter(
            path,
            table.schema,
            use_dictionary=False,
            use_byte_stream_split=True) as writer:
        writer.write_table(table)

    csv_path = path.with_suffix('.csv')
    options = csv.WriteOptions(include_header=True, delimiter='|')
    csv.write_csv(table, csv_path, options)
    fix_csv_nulls(csv_path)


def generate_byte_stream_split_flba(path: Path):
    """FIXED_LEN_BYTE_ARRAY columns, each written twice: PLAIN and BYTE_STREAM_SPLIT.

    Small pages and row groups so that reads span pages and filters skip values.
    """
    import decimal
    num_rows = 1000
    rng = np.random.default_rng(1)
    nulls = rng.random(num_rows) < 0.1

    def dec(precision, scale):
        # random unscaled values with up to `precision` digits, both signs
        values = [None if n else decimal.Decimal(
                      (int.from_bytes(rng.bytes(16), 'little') % 10**precision) * int(rng.choice([-1, 1]))
                  ).scaleb(-scale)
                  for n in nulls]
        return pa.array(values, type=pa.decimal128(precision, scale))

    columns = {
        'id': pa.array(range(num_rows), type=pa.int32()),
        'dec_7_3': dec(7, 3),
        'dec_19_4': dec(19, 4),
        'dec_38_10': dec(38, 10),
        'fixed_16': pa.array([None if n else rng.bytes(16) for n in nulls], type=pa.binary(16)),
        'fixed_20': pa.array([None if n else rng.bytes(20) for n in nulls], type=pa.binary(20)),
        'float16': pa.array(rng.uniform(-100, 100, num_rows).astype(np.float16), type=pa.float16(),
                            mask=nulls),
    }
    arrays, names, bss = [columns['id']], ['id'], []
    for name, array in columns.items():
        if name == 'id':
            continue
        arrays += [array, array]
        names += [f'{name}_plain', f'{name}_bss']
        bss.append(f'{name}_bss')
    table = pa.Table.from_arrays(arrays, names)
    pq.write_table(table, path, use_dictionary=False, use_byte_stream_split=bss,
                   data_page_size=256, row_group_size=400, compression='none')


def fix_csv_nulls(path: Path):
    """ Replace empty values with 'NULL' """
    with open(path, 'r') as f:
        lines = f.readlines()

    with open(path, 'w') as f:
        for line in lines:
            split_line = ["NULL" if val == "" else val for val in line.strip().split("|")]
            f.write("|".join(split_line) + "\n")


if __name__ == '__main__':
    data_dir = Path(__file__).parent
    generate_parquet(data_dir)
