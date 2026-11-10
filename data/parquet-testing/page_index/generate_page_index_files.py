#!/usr/bin/env python3
"""Reference data for the Parquet page index (ColumnIndex / OffsetIndex) tests.

Writes Parquet files with page indexes using Apache Spark (parquet-mr) and pyarrow, and next to every file a CSV with
the page index as parquet-mr reads it (`<name>.expected.csv`). The tests in test/sql/copy/parquet/page_index compare
DuckDB's parquet_page_index() against those CSVs and check that the pages DuckDB skips do not change query results.

The same parquet-mr reader can dump the page index of a file written by DuckDB, and Spark can run a filter over it,
which checks DuckDB's writer against the reference implementation:

    python3 generate_page_index_files.py                      # regenerate the files in this directory
    python3 generate_page_index_files.py --dump f.parquet     # parquet-mr's view of the page index of any file (CSV)
    python3 generate_page_index_files.py --spark-count f.parquet 'id = 5'   # rows Spark finds (column index filtering on)

Requires `pip install pyspark pyarrow` and a JVM (JAVA_HOME). Generated with Spark 4.2.0 (parquet-mr 1.17.0) and
pyarrow 26.0.0.
"""
import argparse
import csv
import datetime
import decimal
import glob
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

CSV_COLUMNS = [
    "row_group_id", "column_id", "path_in_schema", "page_id", "page_offset", "compressed_page_size",
    "first_row_index", "num_rows", "null_page", "min_value", "max_value", "null_count", "boundary_order"
]


#===----------------------------------------------------------------------===//
# Formatting of decoded values in DuckDB's VARCHAR cast format
#===----------------------------------------------------------------------===//
def format_double(value):
    if value != value:
        return "nan"
    if value in (float("inf"), float("-inf")):
        return "inf" if value > 0 else "-inf"
    text = repr(value)
    if text.endswith(".0"):
        text = text[:-2] + ".0"
    return text


def format_timestamp_micros(micros):
    base = datetime.datetime(1970, 1, 1) + datetime.timedelta(microseconds=micros)
    text = base.strftime("%Y-%m-%d %H:%M:%S")
    if base.microsecond:
        text += "." + ("%06d" % base.microsecond).rstrip("0")
    return text


def format_decimal(unscaled, scale):
    value = decimal.Decimal(unscaled).scaleb(-scale)
    return "{:f}".format(value.quantize(decimal.Decimal(1).scaleb(-scale))) if scale > 0 else str(value)


def decode_value(data, primitive_type, logical):
    """Decodes the plain-encoded bytes of a min/max value the way DuckDB prints the statistic."""
    kind = primitive_type.getPrimitiveTypeName().name()
    logical_name = logical.toString() if logical is not None else ""
    if kind == "BOOLEAN":
        return "true" if data[0] else "false"
    if kind == "INT32":
        value = struct.unpack("<i", data)[0]
        if logical_name.startswith("DATE"):
            return (datetime.date(1970, 1, 1) + datetime.timedelta(days=value)).isoformat()
        if logical_name.startswith("DECIMAL"):
            return format_decimal(value, logical.getScale())
        return str(value)
    if kind == "INT64":
        value = struct.unpack("<q", data)[0]
        if logical_name.startswith("TIMESTAMP"):
            unit = logical.getUnit().name()
            if unit == "MILLIS":
                value *= 1000
            elif unit == "NANOS":
                value //= 1000
            # a UTC-adjusted timestamp is a TIMESTAMP WITH TIME ZONE in DuckDB, printed with its offset
            return format_timestamp_micros(value) + ("+00" if logical.isAdjustedToUTC() else "")
        if logical_name.startswith("DECIMAL"):
            return format_decimal(value, logical.getScale())
        return str(value)
    if kind == "FLOAT":
        return format_double(struct.unpack("<f", data)[0])
    if kind == "DOUBLE":
        return format_double(struct.unpack("<d", data)[0])
    if kind == "BINARY":
        return bytes(data).decode("utf-8")
    if kind == "FIXED_LEN_BYTE_ARRAY":
        if logical_name.startswith("DECIMAL"):
            return format_decimal(int.from_bytes(bytes(data), "big", signed=True), logical.getScale())
        return bytes(data).decode("utf-8")
    raise ValueError("unsupported type " + kind)


#===----------------------------------------------------------------------===//
# Spark / parquet-mr
#===----------------------------------------------------------------------===//
def start_spark(extra_conf=None):
    from pyspark.sql import SparkSession
    builder = (SparkSession.builder.master("local[1]").appName("page-index-reference")
               .config("spark.driver.memory", "2g").config("spark.ui.enabled", "false")
               .config("spark.sql.parquet.outputTimestampType", "TIMESTAMP_MICROS")
               .config("spark.sql.session.timeZone", "UTC"))
    for key, value in (extra_conf or {}).items():
        builder = builder.config(key, value)
    spark = builder.getOrCreate()
    spark.sparkContext.setLogLevel("ERROR")
    return spark


def dump_page_index(spark, path):
    """parquet-mr's view of the page index of a file, one row per page as in DuckDB's parquet_page_index()."""
    jvm = spark._jvm
    conf = spark._jsc.hadoopConfiguration()
    input_file = jvm.org.apache.parquet.hadoop.util.HadoopInputFile.fromPath(jvm.org.apache.hadoop.fs.Path(path),
                                                                           conf)
    reader = jvm.org.apache.parquet.hadoop.ParquetFileReader.open(input_file)
    rows = []
    try:
        for row_group_id, block in enumerate(reader.getFooter().getBlocks()):
            num_rows = block.getRowCount()
            for column_id, column in enumerate(block.getColumns()):
                column_index = reader.readColumnIndex(column)
                offset_index = reader.readOffsetIndex(column)
                if column_index is None and offset_index is None:
                    continue
                path_in_schema = ", ".join(column.getPath().toArray())
                primitive_type = column.getPrimitiveType()
                logical = primitive_type.getLogicalTypeAnnotation()
                page_count = offset_index.getPageCount() if offset_index is not None else len(
                    column_index.getNullPages())
                for page_id in range(page_count):
                    row = {"row_group_id": row_group_id, "column_id": column_id, "path_in_schema": path_in_schema,
                           "page_id": page_id}
                    if offset_index is not None:
                        first_row = offset_index.getFirstRowIndex(page_id)
                        next_row = offset_index.getFirstRowIndex(page_id +
                                                                 1) if page_id + 1 < page_count else num_rows
                        row.update(page_offset=offset_index.getOffset(page_id),
                                   compressed_page_size=offset_index.getCompressedPageSize(page_id),
                                   first_row_index=first_row, num_rows=next_row - first_row)
                    else:
                        row.update(page_offset="", compressed_page_size="", first_row_index="", num_rows="")
                    if column_index is not None:
                        null_page = bool(column_index.getNullPages()[page_id])
                        null_counts = column_index.getNullCounts()
                        min_bytes = bytes(column_index.getMinValues().get(page_id).array())
                        max_bytes = bytes(column_index.getMaxValues().get(page_id).array())
                        row.update(null_page="true" if null_page else "false",
                                   min_value="" if null_page else decode_value(min_bytes, primitive_type, logical),
                                   max_value="" if null_page else decode_value(max_bytes, primitive_type, logical),
                                   null_count=null_counts[page_id] if null_counts is not None else "",
                                   boundary_order=column_index.getBoundaryOrder().name())
                    else:
                        row.update(null_page="", min_value="", max_value="", null_count="", boundary_order="")
                    rows.append(row)
    finally:
        reader.close()
    return rows


def write_csv(rows, path):
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def spark_write(spark, df, target, options=None):
    """Writes a DataFrame as a single Parquet file at target (Spark writes a directory of part files)."""
    with tempfile.TemporaryDirectory(dir=os.path.dirname(target)) as tmp:
        out_dir = os.path.join(tmp, "out")
        writer = df.coalesce(1).write.mode("overwrite")
        for key, value in (options or {}).items():
            writer = writer.option(key, value)
        writer.parquet(out_dir)
        part = glob.glob(os.path.join(out_dir, "part-*.parquet"))[0]
        shutil.move(part, target)


def generate_spark_files(spark):
    from pyspark.sql import functions as F

    # 4000 rows, pages of at most 250 rows, so that every column gets several pages per row group.
    # Columns: a sorted key, an unsorted value, strings (some longer than parquet-mr's 64 byte truncation limit),
    # a double, a date, a timestamp, a decimal, a boolean, a nullable column with pages that are entirely NULL,
    # a list and a struct (nested columns: their OffsetIndex counts top-level rows, not values).
    page_options = {"parquet.page.row.count.limit": "250", "parquet.page.size": "4096",
                    "parquet.page.size.row.check.min": "100", "parquet.page.size.row.check.max": "100"}
    df = (spark.range(0, 4000).select(
        F.col("id"),
        ((F.col("id") * 7919) % 1000).cast("int").alias("v"),
        F.when(F.col("id") % 97 == 0, F.concat(F.lit("long_"), F.lpad(F.col("id").cast("string"), 6, "0"),
                                              F.lit("_" + "x" * 80))).otherwise(
            F.concat(F.lit("s_"), F.lpad(F.col("id").cast("string"), 6, "0"))).alias("s"),
        (F.col("id") / 4).cast("double").alias("d"),
        F.date_add(F.lit("2024-01-01").cast("date"), (F.col("id") % 400).cast("int")).alias("dt"),
        F.timestamp_micros(F.lit(1704067200000000) + F.col("id") * 1000000).alias("ts"),
        (F.col("id").cast("decimal(10,2)") / 4).cast("decimal(10,2)").alias("dec"),
        (F.col("id") % 3 == 0).alias("b"),
        F.when((F.col("id") >= 1000) & (F.col("id") < 2000), F.lit(None).cast("int")).when(
            F.col("id") % 5 == 0, F.lit(None).cast("int")).otherwise(F.col("id").cast("int")).alias("n"),
        F.array(F.col("id").cast("int"), (F.col("id") + 1).cast("int")).alias("arr"),
        F.struct(F.col("id").cast("int").alias("a"), F.concat(F.lit("t"), F.col("id").cast("string")).alias("b")).alias("st"),
    ))
    spark_write(spark, df, os.path.join(HERE, "spark_sorted.parquet"), page_options)

    # the same data in descending order of the key: DESCENDING boundary order
    spark_write(spark, df.orderBy(F.col("id").desc()).select("id", "v", "s", "d"),
                os.path.join(HERE, "spark_descending.parquet"), page_options)

    # several row groups (16 KiB blocks) with data page V2 headers
    v2_options = dict(page_options)
    v2_options.update({"parquet.block.size": "16384", "parquet.block.size.row.check.min": "100",
                       "parquet.block.size.row.check.max": "100", "parquet.writer.version": "PARQUET_2_0"})
    spark_write(spark, df.select("id", "v", "s", "n", "dt"), os.path.join(HERE, "spark_row_groups_v2.parquet"),
                v2_options)


def generate_arrow_files():
    import pyarrow as pa
    import pyarrow.parquet as pq

    count = 4000
    ids = list(range(count))
    table = pa.table({
        "id": pa.array(ids, pa.int64()),
        "v": pa.array([(i * 7919) % 1000 for i in ids], pa.int32()),
        "s": pa.array([("long_%06d_" % i) + "y" * 80 if i % 97 == 0 else "s_%06d" % i for i in ids], pa.string()),
        "f": pa.array([i / 8 for i in ids], pa.float32()),
        "dec": pa.array([decimal.Decimal(i) / 4 for i in ids], pa.decimal128(10, 2)),
        "n": pa.array([None if (1000 <= i < 2000 or i % 5 == 0) else i for i in ids], pa.int32()),
        "ts": pa.array([datetime.datetime(2024, 1, 1) + datetime.timedelta(seconds=i) for i in ids], pa.timestamp("us")),
        "arr": pa.array([[i, i + 1] for i in ids], pa.list_(pa.int32())),
    })
    # dictionary encoded with data page V1 headers, plain encoded with data page V2 headers
    pq.write_table(table, os.path.join(HERE, "arrow_sorted.parquet"), write_page_index=True, data_page_size=1024,
                   write_batch_size=128, row_group_size=count, compression="snappy")
    pq.write_table(table, os.path.join(HERE, "arrow_row_groups_v2.parquet"), write_page_index=True,
                   data_page_size=1024, write_batch_size=128, row_group_size=1000, use_dictionary=False,
                   data_page_version="2.0", compression="snappy")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dump", metavar="FILE", help="print parquet-mr's view of the page index of FILE as CSV")
    parser.add_argument("--spark-count", nargs=2, metavar=("FILE", "PREDICATE"),
                        help="count the rows of FILE that match PREDICATE using Spark")
    args = parser.parse_args()

    if args.dump:
        spark = start_spark()
        writer = csv.DictWriter(sys.stdout, fieldnames=CSV_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for row in dump_page_index(spark, os.path.abspath(args.dump)):
            writer.writerow(row)
        spark.stop()
        return
    if args.spark_count:
        path, predicate = args.spark_count
        spark = start_spark()
        print(spark.read.parquet(os.path.abspath(path)).where(predicate).count())
        spark.stop()
        return

    generate_arrow_files()
    spark = start_spark()
    generate_spark_files(spark)
    for path in sorted(glob.glob(os.path.join(HERE, "*.parquet"))):
        write_csv(dump_page_index(spark, path), path[:-len(".parquet")] + ".expected.csv")
    spark.stop()


if __name__ == "__main__":
    main()
