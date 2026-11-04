#!/usr/bin/env python3

import os
import sys
from contextlib import contextmanager
from pathlib import Path


def resolve_duckdb_path(wrapper_path: str | Path, duckdb_name: str) -> Path:
    return Path(wrapper_path).resolve().parent.parent / duckdb_name


def build_run_tests_argv(
    forwarded_args: list[str],
    wrapper_path: str | Path,
    duckdb_name: str = "duckdb",
    test_roots: list[str] | None = None,
) -> list[str]:
    wrapper_dir = Path(wrapper_path).resolve().parent
    build_dir = wrapper_dir.parent
    result = [
        os.fspath(resolve_duckdb_path(wrapper_path, duckdb_name)),
        "--test-extension",
        os.fspath(build_dir / "extension" / "test" / "test.duckdb_extension"),
        "--test-bundle-root",
        os.fspath(build_dir / "test" / "extensions"),
    ]
    for test_root in test_roots or []:
        if test_root:
            result.extend(["--test-root", test_root])
    return [*result, *forwarded_args]


@contextmanager
def pushd(path: str | Path):
    old_cwd = Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old_cwd)


def main(
    forwarded_args: list[str] | None = None,
    *,
    wrapper_path: str | Path | None = None,
    source_root: str | Path | None = None,
    duckdb_name: str = "duckdb",
    test_roots: list[str] | None = None,
) -> int:
    if forwarded_args is None:
        forwarded_args = sys.argv[1:]
    if wrapper_path is None:
        wrapper_path = sys.argv[0]
    if source_root is None:
        raise ValueError("source_root is required")

    source_root_path = Path(source_root).resolve()
    if os.fspath(source_root_path) not in sys.path:
        sys.path.insert(0, os.fspath(source_root_path))

    duckdb_path = resolve_duckdb_path(wrapper_path, duckdb_name)
    if not duckdb_path.is_file():
        print(f"error: expected DuckDB shell at {duckdb_path}", file=sys.stderr)
        return 1

    from scripts.ci import run_tests

    with pushd(source_root_path):
        return int(run_tests.main(build_run_tests_argv(forwarded_args, wrapper_path, duckdb_name, test_roots)) or 0)
