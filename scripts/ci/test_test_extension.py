#!/usr/bin/env python3
"""Run with DUCKDB_TEST_BINARY pointing at a shell built with test extensions."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts.ci import run_tests
from scripts.ci.test_run_tests import REPO_ROOT, protocol_config, write_runner_wrapper


@unittest.skipUnless(os.environ.get("DUCKDB_TEST_BINARY"), "requires DUCKDB_TEST_BINARY")
class TestExtensionIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.binary = str(Path(os.environ["DUCKDB_TEST_BINARY"]).resolve())
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.test_root = self.root / "tests"
        self.test_root.mkdir()
        self.temp_root = self.root / "temp"

    def create_test(self, name, body):
        path = self.test_root / name
        path.write_text(body)
        return "sql:" + path.as_posix()

    def query(self, sql, database=None):
        command = [self.binary, "-unsigned", "-no-agent", "-batch", "-jsonlines"]
        if database is not None:
            command.append(str(database))
        result = subprocess.run(command, input=sql, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]

    def run_selected(self, test_ids):
        flags = [
            "--temp-dir-root",
            str(self.temp_root),
            "--run-id",
            "integration",
            "--temp-dir-run-id",
            "on",
        ]
        sql = (
            "SELECT * FROM run_tests("
            f"{run_tests.sql_string_list(test_ids)}, "
            f"test_roots = {run_tests.sql_string_list([self.test_root])}, "
            f"test_flags = {run_tests.sql_string_list(flags)});"
        )
        rows = self.query(run_tests.build_test_protocol_sql(self.binary, sql))
        self.assertEqual(len(rows), 1, rows)
        return Path(rows[0]["state_database"])

    def test_default_state_database_survives_successful_cleanup(self):
        test_id = self.create_test("pass.test", "query I\nSELECT 42\n----\n42\n")
        database = self.run_selected([test_id])
        self.assertTrue(database.is_file(), database)
        self.assertFalse((self.temp_root / "integration").exists())
        rows = self.query("SELECT test_id, status FROM test_results;", database)
        self.assertEqual(rows, [{"test_id": test_id, "status": "passed"}])

    def test_legacy_unittest_entrypoint_runs_tests_from_generated_extension_roots(self):
        self.create_test(
            "legacy_runner_external_root.test",
            "query I\nSELECT count(*) FROM read_text('CMakeLists.txt')\n----\n1\n",
        )
        build_dir = self.root / "build"
        build_dir.mkdir()
        duckdb_name = Path(self.binary).name
        (build_dir / duckdb_name).symlink_to(self.binary)
        wrapper = build_dir / "test" / ("unittest.py" if os.name == "nt" else "unittest")
        write_runner_wrapper(wrapper, [self.test_root], duckdb_name, legacy=True)
        extension_dir = build_dir / "extension" / "test"
        extension_dir.mkdir(parents=True)
        (extension_dir / "test.duckdb_extension").symlink_to(
            Path(self.binary).parent / "extension" / "test" / "test.duckdb_extension"
        )
        command = [
            sys.executable,
            str(REPO_ROOT / "scripts" / "ci" / "run_tests.py"),
            str(wrapper),
            "--workers",
            "1",
            "--retry",
            "0",
            "--max-retries",
            "0",
            f"--test-flags=--temp-dir-root {self.temp_root}",
            "*legacy_runner_external_root.test",
        ]
        result = subprocess.run(command, cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 passed", result.stdout)

    def test_partial_skip_statistics_are_durable(self):
        test_id = self.create_test(
            "partial.test",
            "mode skip unsupported feature\n\nquery I\nSELECT 1\n----\n2\n\n"
            "mode unskip\n\nquery I\nSELECT 42\n----\n42\n",
        )
        database = self.run_selected([test_id])
        rows = self.query(
            "SELECT status, skip_mode::INTEGER AS skip_mode, partial_skip_reasons FROM test_results;", database
        )
        self.assertEqual(
            rows,
            [
                {
                    "status": "passed",
                    "skip_mode": 1,
                    "partial_skip_reasons": ["mode skip unsupported feature"],
                }
            ],
        )

    def test_cpp_tests_have_separate_temporary_directories(self):
        names = ["Test file operations", "exclusive create fails when the file exists"]
        test_ids = ["cpp:duckdb_tests:" + name for name in names]
        flags = [
            "--temp-dir-root",
            str(self.temp_root),
            "--run-id",
            "integration",
            "--temp-dir-run-id",
            "on",
            "--temp-dir-destroy",
            "never",
        ]
        sql = (
            "SELECT * FROM run_tests("
            f"{run_tests.sql_string_list(test_ids)}, "
            f"extension_roots = {run_tests.sql_string_list([Path(self.binary).parent / 'test/extensions'])}, "
            f"test_flags = {run_tests.sql_string_list(flags)});"
        )
        result = self.query(run_tests.build_test_protocol_sql(self.binary, sql))
        rows = self.query("SELECT status FROM test_results;", result[0]["state_database"])
        self.assertEqual(rows, [{"status": "passed"}, {"status": "passed"}])
        for name in names:
            leaf = "".join(character if character.isalnum() or character in "_-" else "_" for character in name)
            self.assertTrue((self.temp_root / "integration" / leaf).is_dir(), name)

    def test_protocol_body_timeout_preserves_warning_policy(self):
        test_id = self.create_test(
            "slow.test",
            "query I\nSELECT count(*) FROM range(1000000000) a, range(1000000000) b\n----\n0\n",
        )
        config = protocol_config(
            unittest_bin=self.binary,
            test_flags=f"--temp-dir-root {self.temp_root}",
            batch_timeout_seconds=3,
            timeout_policy="warn",
        )
        with mock.patch("scripts.ci.run_tests.TEST_ROOT_OVERRIDES", [self.test_root]):
            result = run_tests.run_batch(config, [test_id])
        self.assertTrue(result["timed_out"], result)
        self.assertTrue(run_tests.can_warn_timeout(config, [test_id], result), result)


if __name__ == "__main__":
    unittest.main()
