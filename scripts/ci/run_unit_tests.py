#!/usr/bin/env python3

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = SCRIPT_DIR.parent.parent


def has_tests(test_file):
    suite = unittest.TestLoader().discover(str(SCRIPT_DIR), pattern=test_file.name)
    return suite.countTestCases() > 0


def run_test_file(test_file, unittest_args):
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "--buffer",
            "--start-directory",
            str(SCRIPT_DIR),
            "--pattern",
            test_file.name,
        ]
        + unittest_args,
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
    )


def main():
    parser = argparse.ArgumentParser(description="Run scripts/ci unit test modules in parallel")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args, unittest_args = parser.parse_known_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    test_files = [test_file for test_file in sorted(SCRIPT_DIR.glob("test_*.py")) if has_tests(test_file)]
    test_count = 0
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(args.jobs, len(test_files))) as executor:
        futures = [executor.submit(run_test_file, test_file, unittest_args) for test_file in test_files]
        results = [future.result() for future in futures]

    for test_file, result in zip(test_files, results):
        summary = re.search(r"Ran (\d+) tests?", result.stderr)
        if summary:
            test_count += int(summary.group(1))
        if result.returncode != 0:
            failures.append(test_file)
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)

    if failures:
        print("Failed CI unit test modules:")
        for test_file in failures:
            print(f"- {test_file.relative_to(REPOSITORY_ROOT)}")
        return 1

    print(f"Ran {test_count} tests across {len(test_files)} modules")
    return 0


if __name__ == "__main__":
    sys.exit(main())
