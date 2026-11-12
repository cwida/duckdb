"""Compare real Windows binaries with and without static-library export directives."""

import argparse
import ctypes
import gzip
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


EXTENSIONS = ["autocomplete", "core_functions", "icu", "json", "parquet", "tpcds", "tpch"]


def snapshot(root):
    binaries = {"cli": root / "duckdb.exe", "dll": root / "src" / "duckdb.dll"}
    binaries.update({name: root / "extension" / name / f"{name}.duckdb_extension" for name in EXTENSIONS})
    result = {}
    for name, path in binaries.items():
        data = path.read_bytes()
        output = subprocess.check_output(["dumpbin", "/nologo", "/exports", str(path)], text=True)
        exports = sorted(re.findall(r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", output, re.MULTILINE))
        result[name] = {
            "bytes": len(data),
            "gzip_bytes": len(gzip.compress(data, compresslevel=9, mtime=0)),
            "exports": exports,
        }
    return result


def compare(before, after):
    required = {"duckdb_open", "duckdb_connect", "duckdb_query"}
    if not required.issubset(after["dll"]["exports"]):
        raise ValueError("duckdb.dll lost its C API exports")
    if before["dll"]["exports"] != after["dll"]["exports"]:
        raise ValueError("duckdb.dll exports changed")
    if "duckdb_open" in after["cli"]["exports"]:
        raise ValueError("The static CLI still exports duckdb_open")
    lines = [
        "| Binary | Bytes before -> after | Reduction | Gzip bytes before -> after | Reduction | Exports before -> after |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for name, current in after.items():
        original = before[name]
        if name != "dll" and current["bytes"] > original["bytes"]:
            raise ValueError(f"{name} grew after removing static exports")
        if name in EXTENSIONS:
            if f"{name}_duckdb_cpp_init" not in current["exports"]:
                raise ValueError(f"{name} lost its entry point")
            if "duckdb_open" in current["exports"]:
                raise ValueError(f"{name} still exports duckdb_open")
            if len(current["exports"]) >= len(original["exports"]):
                raise ValueError(f"{name} did not discard exports")
        reduction = 100 * (1 - current["bytes"] / original["bytes"])
        gzip_reduction = 100 * (1 - current["gzip_bytes"] / original["gzip_bytes"])
        lines.append(
            f"| {name} | {original['bytes']} -> {current['bytes']} | {reduction:.2f}% "
            f"| {original['gzip_bytes']} -> {current['gzip_bytes']} | {gzip_reduction:.2f}% "
            f"| {len(original['exports'])} -> {len(current['exports'])} |"
        )
    return "\n".join(lines) + "\n"


def smoke(root):
    core = (root / "extension/core_functions/core_functions.duckdb_extension").as_posix().replace("'", "''")
    queries = {
        "autocomplete": "SELECT count(*) FROM sql_auto_complete('SEL')",
        "core_functions": "SELECT lower('EXPORTS')",
        "icu": "SELECT count(*) FROM icu_calendar_names()",
        "json": """SELECT json_extract('{"i":42}', '$.i')""",
        "tpcds": "SELECT count(*) FROM tpcds_queries()",
        "tpch": "SELECT count(*) FROM tpch_queries()",
    }
    with tempfile.TemporaryDirectory() as directory:
        parquet = (Path(directory) / "exports.parquet").as_posix().replace("'", "''")
        queries["parquet"] = (
            f"COPY (SELECT 42 AS i) TO '{parquet}' (FORMAT PARQUET); SELECT i FROM read_parquet('{parquet}')"
        )
        for name, query in queries.items():
            extension = (root / "extension" / name / f"{name}.duckdb_extension").as_posix().replace("'", "''")
            sql = f"LOAD '{core}'; "
            if name != "core_functions":
                sql += f"LOAD '{extension}'; "
            sql += (
                "SELECT CASE WHEN count(*) = 1 THEN true ELSE error('Extension did not load dynamically') END "
                f"FROM duckdb_extensions() WHERE extension_name = '{name}' AND loaded AND install_mode = 'NOT_INSTALLED'; "
            )
            subprocess.run([str(root / "duckdb.exe"), "-bail", "-unsigned", "-c", sql + query], check=True)
    dll = ctypes.CDLL(str(root / "src/duckdb.dll"))
    dll.duckdb_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    dll.duckdb_open.restype = ctypes.c_int
    dll.duckdb_connect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    dll.duckdb_connect.restype = ctypes.c_int
    dll.duckdb_query.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
    dll.duckdb_query.restype = ctypes.c_int
    dll.duckdb_disconnect.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    dll.duckdb_disconnect.restype = None
    dll.duckdb_close.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    dll.duckdb_close.restype = None
    database, connection = ctypes.c_void_p(), ctypes.c_void_p()
    try:
        if dll.duckdb_open(None, ctypes.byref(database)) or dll.duckdb_connect(database, ctypes.byref(connection)):
            raise RuntimeError("The DuckDB DLL C API failed to open a connection")
        if dll.duckdb_query(connection, b"CREATE TABLE export_smoke(i INTEGER)", None):
            raise RuntimeError("The DuckDB DLL C API failed to execute SQL")
    finally:
        if connection:
            dll.duckdb_disconnect(ctypes.byref(connection))
        if database:
            dll.duckdb_close(ctypes.byref(database))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--smoke", action="store_true")
    arguments = parser.parse_args()
    after = snapshot(arguments.root)
    arguments.output.write_text(json.dumps(after, indent=2) + "\n", encoding="utf-8")
    if arguments.compare:
        summary = compare(json.loads(arguments.compare.read_text(encoding="utf-8")), after)
        print(summary)
        if os.getenv("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
                output.write(summary)
    if arguments.smoke:
        smoke(arguments.root)


if __name__ == "__main__":
    main()
