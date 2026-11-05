#!/usr/bin/env python3
"""Run the helper tests in LLDB's Python interpreter, optionally with line coverage."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unittest", type=Path, default=Path("build/reldebug/test/unittest"))
    parser.add_argument("--lldb", default="lldb")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--coverage-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    executable = args.unittest.resolve()
    if not executable.is_file():
        parser.error("build the unittest executable or pass --unittest <path>")
    tests = Path(__file__).resolve().parent
    coverage_dir = str(args.coverage_dir.resolve()) if args.coverage_dir else None
    with tempfile.TemporaryDirectory(prefix="duckdb-lldb-") as temporary:
        fixture = Path(temporary) / "values"
        report = Path(temporary) / "result.json"
        subprocess.run(
            [
                args.cxx,
                "-std=c++17",
                "-g",
                "-O0",
                "-I",
                str(root / "src/include"),
                str(tests / "values.cpp"),
                "-o",
                str(fixture),
            ],
            check=True,
            cwd=root,
        )
        run = "script test_helpers.run(lldb.debugger, {!r}, {!r}, {!r}, {!r})".format(
            str(executable), str(fixture), str(report), coverage_dir
        )
        completed = subprocess.run(
            [
                args.lldb,
                "--no-lldbinit",
                "--batch",
                "-o",
                "command script import " + shlex.quote(str(tests / "test_helpers.py")),
                "-o",
                run,
            ],
            cwd=root,
            timeout=180,
        )
        if completed.returncode or not report.exists():
            raise SystemExit("LLDB did not complete the tests")
        result = json.loads(report.read_text())
        print(result["output"])
        raise SystemExit(0 if result["successful"] else 1)


if __name__ == "__main__":
    main()
