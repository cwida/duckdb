#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
from pathlib import Path


RELEASE_VERSION_FILE = Path(__file__).with_name("release_version.txt")
VERSION_PATTERN = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|rc|dev)[0-9]+)?$")


def release_version() -> str:
    version = RELEASE_VERSION_FILE.read_text(encoding="utf8").strip()
    if re.fullmatch(r"[0-9]+\.[0-9]+", version) is None:
        raise ValueError(f"Invalid release version '{version}' in {RELEASE_VERSION_FILE}")
    return version


def commit_count() -> int:
    try:
        return int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"], text=True).strip())
    except (OSError, subprocess.CalledProcessError, ValueError):
        return 0


def next_patch_version(base_version: str) -> int:
    tag_pattern = re.compile(rf"^v{re.escape(base_version)}\.([0-9]+)$")
    tag_output = subprocess.check_output(["git", "tag", "--merged", "HEAD", "--list", f"v{base_version}.*"], text=True)
    patch_versions = []
    for tag in tag_output.splitlines():
        match = tag_pattern.fullmatch(tag)
        if match:
            patch_versions.append(int(match.group(1)))
    return max(patch_versions, default=-1) + 1


def resolve_version(explicit_version: str = "", alpha_run_number: str = "") -> str:
    if explicit_version:
        if VERSION_PATTERN.fullmatch(explicit_version) is None:
            raise ValueError(f"Invalid DuckDB version '{explicit_version}'")
        return explicit_version
    base_version = release_version()
    patch_version = next_patch_version(base_version)
    if alpha_run_number:
        return f"v{base_version}.{patch_version}-alpha{alpha_run_number}"
    return f"v{base_version}.{patch_version}-dev{commit_count()}"


def is_release_build(version: str) -> bool:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise ValueError(f"Invalid DuckDB version '{version}'")
    return "-rc" not in version and "-dev" not in version


def environment_version(explicit_version: str = "") -> str:
    return explicit_version or os.getenv("DUCKDB_VERSION", "") or os.getenv("OVERRIDE_GIT_DESCRIBE", "")


def resolve_commit(explicit_commit: str = "") -> str:
    commit = explicit_commit
    if not commit:
        try:
            commit = subprocess.check_output(["git", "log", "-1", "--format=%H"], text=True).strip()
        except (OSError, subprocess.CalledProcessError):
            commit = "0123456789"
    if re.fullmatch(r"[a-fA-F0-9]{10,64}", commit) is None:
        raise ValueError(f"Invalid DuckDB commit '{commit}'")
    return commit.lower()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb-commit", default=os.getenv("DUCKDB_COMMIT", ""))
    parser.add_argument("--duckdb-version", default="")
    parser.add_argument("--alpha-run-number", default="")
    parser.add_argument("--github-output", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    commit = resolve_commit(args.duckdb_commit)
    version = resolve_version(environment_version(args.duckdb_version), args.alpha_run_number)
    if args.github_output:
        print(f"duckdb_commit={commit}")
        print(f"duckdb_version={version}")
        print(f"is_release_build={'true' if is_release_build(version) else 'false'}")
    else:
        print(version)


if __name__ == "__main__":
    main()
