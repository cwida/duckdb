import pathlib

import pyarrow as pa
import pyarrow.parquet as pq


def generate(directory):
    for name, nullable, dictionary, small_pages in [
        ("required_dictionary_page_boundary", False, True, True),
        ("nullable_dictionary_page_boundary", True, True, True),
        ("required_plain_page_boundary", False, False, True),
        ("required_dictionary_single_page", False, True, False),
    ]:
        schema = pa.schema([pa.field("x", pa.int64(), nullable=nullable)])
        table = pa.Table.from_arrays(
            [pa.array([1] * 6001, type=pa.int64())], schema=schema
        )
        pq.write_table(
            table,
            directory / (name + ".parquet"),
            row_group_size=6001,
            use_dictionary=dictionary,
            compression="NONE",
            write_statistics=False,
            data_page_size=1 if small_pages else 1048576,
            write_batch_size=1000 if small_pages else 6001,
        )


if __name__ == "__main__":
    generate(pathlib.Path(__file__).resolve().parent)
