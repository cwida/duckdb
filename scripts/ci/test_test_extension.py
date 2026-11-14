#!/usr/bin/env python3
"""Run with DUCKDB_TEST_BINARY pointing at a shell built with test extensions."""

import json
import os
from pathlib import Path
import shutil
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

    def test_downloaded_test_extension_uses_the_current_build_directory(self):
        build_dir = self.root / "build" / "release"
        build_dir.mkdir(parents=True)
        binary = build_dir / Path(self.binary).name
        shutil.copy2(self.binary, binary)
        repository = build_dir / "repository" / "version" / "platform"
        repository.mkdir(parents=True)
        shutil.copy2(
            Path(self.binary).parent / "extension" / "test" / "test.duckdb_extension",
            repository / "test.duckdb_extension",
        )
        self.create_test(
            "current_build_directory.test",
            f"query T\nSELECT '{{BUILD_DIRECTORY}}'\n----\n{build_dir.resolve()}\n",
        )
        result = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "scripts" / "ci" / "run_tests.py"),
                str(binary),
                "--test-root",
                str(self.test_root),
                "--workers",
                "1",
                "--retry",
                "0",
                f"--test-flags=--temp-dir-root {self.temp_root}",
                "*",
            ],
            cwd=self.root,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 passed", result.stdout)

    def test_extension_ci_runs_only_configured_project_and_extension_tests(self):
        (self.root / "project.txt").write_text("project")
        project_test = self.root / "test" / "project.test"
        sqlite_test = self.root / "third_party" / "sqllogictest" / "test" / "sqlite.test"
        extension_root = self.root / "extension" / "test" / "sql"
        extension_test = extension_root / "extension.test"
        for path in [project_test, sqlite_test, extension_test]:
            path.parent.mkdir(parents=True, exist_ok=True)
        project_test.write_text("query I\nSELECT count(*) FROM read_text('project.txt')\n----\n1\n")
        sqlite_test.write_text("query I\nSELECT 42\n----\n42\n")
        (extension_root.parent.parent / "extension.txt").write_text("extension")
        extension_test.write_text("query I\nSELECT count(*) FROM read_text('extension.txt')\n----\n1\n")

        build_dir = self.root / "build"
        build_dir.mkdir()
        duckdb_name = Path(self.binary).name
        (build_dir / duckdb_name).symlink_to(self.binary)
        wrapper = build_dir / "test" / ("unittest.py" if os.name == "nt" else "unittest")
        write_runner_wrapper(wrapper, [extension_root], duckdb_name, test_root_directory=self.root)
        extension_dir = build_dir / "extension" / "test"
        extension_dir.mkdir(parents=True)
        (extension_dir / "test.duckdb_extension").symlink_to(
            Path(self.binary).parent / "extension" / "test" / "test.duckdb_extension"
        )
        expected_names = {
            path.relative_to(self.root).as_posix() for path in [project_test, sqlite_test, extension_test]
        }
        commands = [
            [sys.executable, str(wrapper)],
            [sys.executable, str(REPO_ROOT / "scripts" / "ci" / "run_tests.py"), str(wrapper)],
        ]
        for command in commands:
            with self.subTest(command=command):
                result = subprocess.run(
                    [*command, "--list-tests", "*"], cwd=REPO_ROOT, capture_output=True, text=True, timeout=60
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                listed_names = {line.split("\t")[0] for line in result.stdout.splitlines()[1:]}
                self.assertEqual(listed_names, expected_names, result.stdout)
                result = subprocess.run(
                    [
                        *command,
                        "--workers",
                        "1",
                        "--retry",
                        "0",
                        "--max-retries",
                        "0",
                        f"--test-flags=--temp-dir-root {self.temp_root}",
                        "*",
                    ],
                    cwd=REPO_ROOT,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("3 passed, 0 skipped", result.stdout)

    def test_partial_skip_statistics_are_durable(self):
        test_id = self.create_test(
            "partial.test",
            "mode skip unsupported feature\n\nquery I\nSELECT 1\n----\n2\n\n"
            "mode unskip\n\nquery I\nSELECT 42\n----\n42\n",
        )
        database = self.run_selected([test_id])
        rows, error = run_tests.read_test_state(protocol_config(unittest_bin=self.binary), database, os.environ.copy())
        self.assertIsNone(error)
        self.assertEqual(
            [
                {
                    "status": row["status"],
                    "skip_mode": int(row["skip_mode"]),
                    "partial_skip_reasons": row["partial_skip_reasons"],
                }
                for row in rows
            ],
            [
                {
                    "status": "passed",
                    "skip_mode": 1,
                    "partial_skip_reasons": ["mode skip unsupported feature"],
                }
            ],
        )

    def test_result_recovery_preserves_failure_messages_and_empty_lists(self):
        passed_id = self.create_test("passed.test", "query I\nSELECT 42\n----\n42\n")
        failed_id = self.create_test("failed.test", "query I\nSELECT 42\n----\n43\n")
        database = self.run_selected([passed_id, failed_id])
        rows, error = run_tests.read_test_state(protocol_config(unittest_bin=self.binary), database, os.environ.copy())
        self.assertIsNone(error)
        rows_by_id = {row["test_id"]: row for row in rows}
        self.assertEqual(rows_by_id[passed_id]["status"], "passed")
        self.assertEqual(rows_by_id[passed_id]["failures"], [])
        self.assertEqual(rows_by_id[failed_id]["status"], "failed")
        self.assertTrue(rows_by_id[failed_id]["failures"])
        for row in rows:
            self.assertEqual(row["partial_skip_reasons"], [])

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

    def test_batch_timeout_identifies_the_active_test_before_an_unstarted_skip(self):
        first = self.create_test("first.test", "query I\nSELECT 42\n----\n42\n")
        hanging = self.create_test(
            "hanging.test",
            "query I\nSELECT count(*) FROM range(1000000000) a, range(1000000000) b\n----\n0\n",
        )
        skipped = self.create_test("skipped.test", "require-env DUCKDB_CI_MISSING_TEST_VARIABLE\n")
        batch = [first, hanging, skipped]
        config = protocol_config(
            unittest_bin=self.binary,
            test_flags=f"--temp-dir-root {self.temp_root}",
            batch_timeout_seconds=3,
        )
        with mock.patch("scripts.ci.run_tests.TEST_ROOT_OVERRIDES", [self.test_root]):
            result = run_tests.run_batch(config, batch)
        self.assertTrue(result["timed_out"], result)
        failure = run_tests.parse_failure_info(result["message"], result["stdout"], result["stderr"], batch)
        self.assertEqual(failure.test_name, hanging)
        self.assertEqual(failure.reproduce_batch, [hanging])
        self.assertIn("incomplete tests:", failure.detail_lines)


if __name__ == "__main__":
    unittest.main()
