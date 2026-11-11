#!/usr/bin/env python3
import os
import json
import shlex
import sys
import tempfile
import textwrap

import threading
import unittest
from io import BytesIO, StringIO, TextIOWrapper
from unittest import mock
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.ci import run_tests
from scripts.ci import test_runner_wrapper


def create_temp_file(content: str, **format_vars: object) -> Path:
    rendered_content = textwrap.dedent(content).lstrip().format(**format_vars)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", delete=False) as tmp_file:
        tmp_file.write(rendered_content)
        tmp_file.flush()
        return Path(tmp_file.name)


def start_runner(cli_args: list[str]):
    return run_tests.invoke(cli_args, cwd=REPO_ROOT)


def strip_ansi_lines(lines: list[str]) -> list[str]:
    return [run_tests.strip_ansi(line) for line in lines]


class RunTestsScriptTest(unittest.TestCase):
    def run_timeout_fixture(self, modes, *args):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            helper = root / "child.py"
            helper.write_text(
                textwrap.dedent(
                    """\
                    import json, os, signal, sys, time
                    from pathlib import Path

                    root = Path(__file__).parent
                    name = Path(sys.argv[1]).read_text().strip()
                    mode = Path(name).stem
                    attempts = root / (mode + ".attempts")
                    attempt = int(attempts.read_text()) + 1 if attempts.exists() else 1
                    attempts.write_text(str(attempt))

                    def event(kind, status="ok"):
                        value = {"event": kind, "name": name, "status": status}
                        if kind == "begin" and mode != "old":
                            value["version"] = 2
                        if status == "skip-requirement":
                            value["data"] = "require missing"
                        print("[TEST_EVENT] " + json.dumps(value), file=sys.stderr, flush=True)

                    def hang():
                        time.sleep(60)

                    if mode == "startup":
                        hang()
                    event("begin")
                    if mode == "initialization":
                        hang()
                    event("execution-begin")
                    if mode in ("slow", "old"):
                        hang()
                    if mode == "malformed":
                        print('[TEST_EVENT] {"event":', file=sys.stderr, flush=True)
                        hang()
                    if mode == "wrong_name":
                        event("begin")
                        hang()
                    if mode == "worker_error":
                        event("error", "error")
                        hang()
                    if mode in ("cleanup", "caught_error_cleanup", "after_end"):
                        event("execution-complete", "error" if mode == "caught_error_cleanup" else "ok")
                        if mode == "after_end":
                            event("end")
                        hang()
                    if mode in ("wrong_result", "wrong_result_hang") or (
                        mode in ("flaky", "failure_then_timeout") and attempt == 1
                    ):
                        print("Error: Wrong result in query!", file=sys.stderr, flush=True)
                        if mode.endswith("_hang"):
                            hang()
                        sys.exit(1)
                    if mode == "failure_then_timeout":
                        hang()
                    if mode == "assert_hang":
                        print("Assertion failed: value != nullptr", file=sys.stderr, flush=True)
                        hang()
                    if mode == "signal":
                        os.kill(os.getpid(), signal.SIGTERM)
                    status = "skip-requirement" if mode == "skip" else "ok"
                    event("execution-complete", status)
                    event("end", status)
                    """
                )
            )
            test_list = root / "tests"
            test_list.write_text("".join(f"test/sql/{mode}.test\n" for mode in modes))
            summary = root / "summary.md"
            with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary), "GITHUB_SHA": "test-revision"}):
                proc = start_runner(
                    [
                        "--test-list",
                        str(test_list),
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--batch-timeout",
                        "0.5",
                        "--max-retries",
                        "0",
                        "--timeout-policy",
                        "warn",
                        "--test-command",
                        f"{shlex.quote(sys.executable)} {shlex.quote(str(helper))} {{test_list}}",
                        "unused-binary",
                        *args,
                    ]
                )
            attempts = {path.stem: int(path.read_text()) for path in root.glob("*.attempts")}
            return proc, attempts, summary.read_text() if summary.exists() else ""

    def test_timeout_warning_continues_and_reports_exact_counts(self):
        proc, attempts, summary = self.run_timeout_fixture(["slow", "pass", "skip"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(attempts, {"slow": 1, "pass": 1, "skip": 1})
        self.assertIn("1 passed, 1 skipped, 1 timed out", proc.stdout)
        self.assertNotIn("all tests passed", proc.stdout)
        self.assertIn("reproduce: unused-binary test/sql/slow.test", proc.stdout)
        self.assertIn("test-revision", summary)
        self.assertIn("`test/sql/slow.test`", summary)

    def test_default_timeout_policy_still_fails(self):
        proc, _, summary = self.run_timeout_fixture(["slow"], "--timeout-policy", "fail")
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("0 passed, 1 failed", proc.stdout)
        self.assertEqual(summary, "")
        self.assertEqual(run_tests.parse_args([]).timeout_policy, "fail")

    def test_timeout_warning_does_not_hide_other_failures(self):
        proc, attempts, _ = self.run_timeout_fixture(["slow", "wrong_result", "pass"])
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(len(attempts), 3)
        self.assertIn("1 passed, 1 failed, 0 skipped, 1 timed out", proc.stdout)
        self.assertIn("reproduce all failed tests:", proc.stdout)

    def test_timeout_warning_requires_active_body_without_errors(self):
        for mode in (
            "startup",
            "initialization",
            "old",
            "malformed",
            "wrong_name",
            "cleanup",
            "caught_error_cleanup",
            "after_end",
            "wrong_result_hang",
            "assert_hang",
            "signal",
            "worker_error",
        ):
            with self.subTest(mode=mode):
                proc, _, summary = self.run_timeout_fixture([mode])
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn("0 passed, 1 failed", proc.stdout)
                self.assertEqual(summary, "")

    def test_timeout_warning_preserves_retry_budget_for_failures(self):
        proc, attempts, _ = self.run_timeout_fixture(["slow", "flaky"], "--retry", "1", "--max-retries", "1")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(attempts, {"slow": 1, "flaky": 2})
        self.assertIn("retrying failed test test/sql/flaky.test", proc.stdout)
        self.assertIn("1 passed, 0 skipped, 1 timed out", proc.stdout)

    def test_timeout_on_retry_does_not_hide_original_failure(self):
        proc, attempts, summary = self.run_timeout_fixture(
            ["failure_then_timeout"], "--retry", "1", "--max-retries", "1"
        )
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(attempts, {"failure_then_timeout": 2})
        self.assertEqual(summary, "")

    def test_timeout_warning_counts_multiple_workers(self):
        proc, attempts, _ = self.run_timeout_fixture(["slow", "pass", "skip"], "--workers", "3")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(attempts), 3)
        self.assertIn("1 passed, 1 skipped, 1 timed out", proc.stdout)

    def test_timeout_warning_does_not_count_unlaunched_tests_as_passed(self):
        proc, attempts, _ = self.run_timeout_fixture(["wrong_result", "pass"], "--fail-fast")
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertEqual(attempts, {"wrong_result": 1})
        self.assertIn("0 passed, 1 failed, 0 skipped, 1 not run", proc.stdout)

    def test_timeout_warning_rejects_multi_test_batches(self):
        proc, attempts, _ = self.run_timeout_fixture(["slow"], "--batch-size", "2")
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("requires --batch-size 1", proc.stdout)
        self.assertEqual(attempts, {})

    def test_timeout_warning_rejects_stabilization(self):
        for args in (["--stabilize-tests"], ["--changed-tests", "unused-test-list"]):
            with self.subTest(args=args):
                proc, attempts, _ = self.run_timeout_fixture(["slow"], *args)
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn("cannot be combined with test stabilization", proc.stdout)
                self.assertEqual(attempts, {})

    def test_cancellation_is_not_a_timeout_warning(self):
        timer = threading.Timer(0.2, run_tests.STOP_REQUESTED.set)
        timer.start()
        try:
            proc, _, summary = self.run_timeout_fixture(["slow"], "--batch-timeout", "30")
            self.assertEqual(proc.returncode, 130, proc.stdout + proc.stderr)
            self.assertEqual(summary, "")
        finally:
            timer.cancel()
            timer.join()
            run_tests.STOP_REQUESTED.clear()

    def test_line_buffering_escapes_unicode_for_non_utf8_streams(self):
        stdout_buffer = BytesIO()
        stderr_buffer = BytesIO()
        stdout = TextIOWrapper(stdout_buffer, encoding="cp1252")
        stderr = TextIOWrapper(stderr_buffer, encoding="cp1252")

        with (
            mock.patch("scripts.ci.run_tests.sys.stdout", stdout),
            mock.patch("scripts.ci.run_tests.sys.stderr", stderr),
        ):
            run_tests.enable_line_buffering()
            print("❌ ran tests", file=run_tests.sys.stdout)
            print("❌ failed", file=run_tests.sys.stderr)
            run_tests.sys.stdout.flush()
            run_tests.sys.stderr.flush()

        self.assertEqual(stdout_buffer.getvalue(), b"\\u274c ran tests\n")
        self.assertEqual(stderr_buffer.getvalue(), b"\\u274c failed\n")

    def test_highlight_stack_trace_lines_colors_frame_index_and_function_name(self):
        lines = run_tests.highlight_stack_trace_lines(
            [
                "INTERNAL Error: something bad happened",
                "Stack Trace:",
                "0        duckdb::Exception::Exception(duckdb::ExceptionType) + 288",
                "1        duckdb::TaskScheduler::ExecuteForever(std::__1::atomic<bool>*, duckdb::TaskSchedulerType) + 868",
                "2        Catch::RunContext::invokeActiveTestCase() + 144",
                "3        main(int, char**) + 32",
            ]
        )

        self.assertEqual(
            lines[2],
            (
                f"{run_tests.ANSI_DARK_GRAY}0{run_tests.ANSI_RESET}        "
                f"{run_tests.ANSI_TEAL}duckdb::Exception::Exception{run_tests.ANSI_RESET}"
                "(duckdb::ExceptionType) + 288"
            ),
        )
        self.assertEqual(
            lines[3],
            (
                f"{run_tests.ANSI_DARK_GRAY}1{run_tests.ANSI_RESET}        "
                f"{run_tests.ANSI_TEAL}duckdb::TaskScheduler::ExecuteForever{run_tests.ANSI_RESET}"
                "(std::__1::atomic<bool>*, duckdb::TaskSchedulerType) + 868"
            ),
        )
        self.assertEqual(
            strip_ansi_lines(lines[2:]),
            [
                "0        duckdb::Exception::Exception(duckdb::ExceptionType) + 288",
                "1        duckdb::TaskScheduler::ExecuteForever(std::__1::atomic<bool>*, duckdb::TaskSchedulerType) + 868",
            ],
        )
        self.assertEqual(len(lines), 4)

    def test_wrapper_builds_sibling_unittest_argv(self):
        argv = test_runner_wrapper.build_run_tests_argv(["[slow]", "test/sql/a.test"], "build/release/test/run")
        self.assertEqual(
            argv, [str((REPO_ROOT / "build" / "release" / "test" / "unittest").resolve()), "[slow]", "test/sql/a.test"]
        )

    def test_wrapper_uses_windows_executable_name(self):
        argv = test_runner_wrapper.build_run_tests_argv([], r"test\run.py", "unittest.exe")
        self.assertEqual(argv, [str(Path(r"test\run.py").resolve().parent / "unittest.exe")])

    def test_wrapper_errors_when_sibling_binary_is_missing(self):
        wrapper_dir = Path(tempfile.mkdtemp())
        try:
            with mock.patch("scripts.ci.test_runner_wrapper.sys.stderr", new=StringIO()) as stderr:
                rc = test_runner_wrapper.main([], wrapper_path=wrapper_dir / "run", source_root=REPO_ROOT)
            self.assertEqual(rc, 1)
            self.assertIn("expected sibling unittest binary", stderr.getvalue())
        finally:
            os.rmdir(wrapper_dir)

    def test_wrapper_invokes_run_tests_from_source_root(self):
        wrapper_dir = Path(tempfile.mkdtemp())
        unittest_path = wrapper_dir / "unittest"
        unittest_path.write_text("", encoding="utf8")
        original_cwd = Path.cwd()
        try:
            with mock.patch("scripts.ci.run_tests.main", return_value=7) as mock_main:
                rc = test_runner_wrapper.main(
                    ["--workers", "1", "test/sql/a.test"], wrapper_path=wrapper_dir / "run", source_root=REPO_ROOT
                )
            self.assertEqual(rc, 7)
            self.assertEqual(Path.cwd(), original_cwd)
            mock_main.assert_called_once_with([str(unittest_path.resolve()), "--workers", "1", "test/sql/a.test"])
        finally:
            unittest_path.unlink(missing_ok=True)
            os.rmdir(wrapper_dir)

    def test_failed_tests_reproducer_uses_runner_and_preserves_context(self):
        command = run_tests.format_failed_tests_reproducer(
            "/repo/build/reldebug/test/unittest",
            "--force-storage --label 'two words'",
            ["test/configs/a config.json"],
            ["test/sql/a.test", "Compiled, test's name"],
        )

        self.assertEqual(
            shlex.split(command),
            [
                os.path.relpath("/repo/build/reldebug/test/run", REPO_ROOT),
                "--test-flags",
                "--force-storage --label 'two words'",
                "--test-config",
                "test/configs/a config.json",
                '"test/sql/a.test","Compiled\\, test\'s name"',
            ],
        )

    def test_batch_filter_preserves_literal_sql_paths_and_escapes_catch_names(self):
        cases = [
            (
                ["test/sql/a,b.test", "test/sql/c,d.test_slow", "test/sql/e,f.test_coverage"],
                "test/sql/a,b.test\ntest/sql/c,d.test_slow\ntest/sql/e,f.test_coverage\n",
            ),
            (
                ["test/sql/a,b.test", "Compiled, test"],
                '"test/sql/a\\,b.test"\n"Compiled\\, test"\n',
            ),
            (
                ["Compiled, test", "test/sql/a,b.test"],
                '"Compiled\\, test"\n"test/sql/a\\,b.test"\n',
            ),
            (
                ['Compiled "quoted", test', r"Compiled\test"],
                '"Compiled \\"quoted\\"\\, test"\n"Compiled\\\\test"\n',
            ),
        ]
        for names, expected in cases:
            with self.subTest(names=names):
                test_list = create_temp_file("{names}", names="\n".join(names))
                helper = create_temp_file(
                    """
                    import sys
                    from pathlib import Path
                    actual = Path(sys.argv[1]).read_text()
                    assert actual == {expected!r}, repr(actual)
                    """,
                    expected=expected,
                )
                try:
                    proc = start_runner(
                        [
                            "unused-binary",
                            "--test-list",
                            str(test_list),
                            "--workers",
                            "1",
                            "--retry",
                            "0",
                            "--test-command",
                            f"{shlex.quote(sys.executable)} {shlex.quote(str(helper))} {{test_list}}",
                        ]
                    )
                    self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                finally:
                    test_list.unlink(missing_ok=True)
                    helper.unlink(missing_ok=True)

    def test_invalid_catch_filter_is_not_counted_as_a_pass(self):
        test_list = create_temp_file("compiled,test\n")
        helper = create_temp_file(
            """
            print('Invalid Filter: "compiled,test"')
            print('All tests passed (1 assertion in 1 test case)')
            """
        )
        try:
            proc = start_runner(
                [
                    "unused-binary",
                    "--test-list",
                    str(test_list),
                    "--workers",
                    "1",
                    "--retry",
                    "2",
                    "--test-command",
                    f"{shlex.quote(sys.executable)} {shlex.quote(str(helper))} {{test_list}}",
                ]
            )
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn('Invalid Filter: "compiled,test"', proc.stdout)
            self.assertNotIn("retrying failed test", proc.stdout)
            self.assertNotIn("ran tests: 1 passed", proc.stdout)
        finally:
            test_list.unlink(missing_ok=True)
            helper.unlink(missing_ok=True)

    def test_summarizes_wrong_result_failure(self):
        stderr = """
1. test/sql/parallelism/interquery/concurrent_writes_during_index_creation.test_slow:29
================================================================================

Error: Wrong result in query! (test/sql/parallelism/interquery/concurrent_writes_during_index_creation.test_slow:29)!
================================================================================
SELECT COUNT(*) FROM integers WHERE i = 1;
================================================================================
Mismatch on row 1, column count_star()(index 1)
16 <> 20001
================================================================================
Expected result:
================================================================================
20001

================================================================================
Actual result:
================================================================================
16
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(
            None,
            "",
            stderr,
            ["test/sql/parallelism/interquery/concurrent_writes_during_index_creation.test_slow"],
        )
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL test/sql/parallelism/interquery/concurrent_writes_during_index_creation.test_slow",
                "",
                "",
                "  > 29  query I",
                "    30  SELECT COUNT(*) FROM integers WHERE i = 1",
                "    31  ----",
                "    32  20001",
                "",
                "details: Mismatch on row 1, column count_star()(index 1)",
                "",
                "Expected result:",
                "20001",
                "",
                "Actual result:",
                "16",
            ],
        )
        self.assertEqual(
            reproduce_batch,
            ["test/sql/parallelism/interquery/concurrent_writes_during_index_creation.test_slow"],
        )

    def test_reports_skipped_tests_summary(self):
        cases = [
            {
                "name": "plain",
                "stdout": """
All tests passed (5 skipped tests, 123 assertions in 4 test cases)
[TEST_EVENT] {"event":"end","name":"test/sql/a.test","status":"ok","passes":4,"fails":0,"skip-mode":2,"partial-skip-reasons":["mode skip flaky planner","mode skip unsupported"]}
[TEST_EVENT] {"event":"end","name":"test/sql/b.test","status":"ok","passes":7,"fails":0,"skip-mode":1,"partial-skip-reasons":["mode skip flaky planner"]}

Skipped tests for the following reasons:
require longdouble: 2
require-env SOME_TOKEN: 3
mode skip flaky planner: 2
mode skip unsupported: 1
""",
                "expected_skip_count": 5,
                "expected_reasons": [
                    "require longdouble: 2",
                    "require-env SOME_TOKEN: 3",
                ],
                "expected_partial_count": 2,
                "expected_partial_reasons": [
                    "mode skip flaky planner: 2",
                    "mode skip unsupported: 1",
                ],
            },
            {
                "name": "ansi",
                "stdout": (
                    "\x1b[36mAll tests passed (14 skipped tests, 2659 assertions in 86 test cases)\x1b[0m\n\n"
                    "\x1b[1mSkipped tests for the following reasons:\x1b[0m\n"
                    "\x1b[33mrequire httpfs: 1\x1b[0m\n"
                    "\x1b[33mrequire icu: 2\x1b[0m\n"
                    "\x1b[33mrequire-env LOCAL_EXTENSION_REPO: 5\x1b[0m\n"
                    "\x1b[33mmode skip flaky parser: 2\x1b[0m\n"
                    '[TEST_EVENT] {"event":"end","name":"test/sql/a.test","status":"ok","passes":4,'
                    '"fails":0,"skip-mode":1,"partial-skip-reasons":["mode skip flaky parser"]}\n'
                    '[TEST_EVENT] {"event":"end","name":"test/sql/b.test","status":"ok","passes":7,'
                    '"fails":0,"skip-mode":1,"partial-skip-reasons":["mode skip flaky parser"]}\n'
                ),
                "expected_skip_count": 14,
                "expected_reasons": [
                    "require httpfs: 1",
                    "require icu: 2",
                    "require-env LOCAL_EXTENSION_REPO: 5",
                ],
                "expected_partial_count": 2,
                "expected_partial_reasons": ["mode skip flaky parser: 2"],
            },
        ]

        for case in cases:
            with self.subTest(case=case["name"]):
                test_list_path = create_temp_file("test/sql/a.test\n")
                try:
                    with mock.patch(
                        "scripts.ci.run_tests.run_batch",
                        return_value={
                            "failed": False,
                            "stdout": case["stdout"],
                            "stderr": "",
                            "message": None,
                            "peak_rss_bytes": 0,
                        },
                    ):
                        proc = start_runner(
                            [
                                "--workers",
                                "1",
                                "--batch-size",
                                "1",
                                "--test-list",
                                str(test_list_path),
                                "--test-command",
                                "echo fake-run {test_list}",
                                "unused-binary",
                            ]
                        )
                finally:
                    test_list_path.unlink(missing_ok=True)

                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn("ran tests: ", proc.stdout)
                self.assertIn(
                    f", {case['expected_skip_count']} skipped ({case['expected_partial_count']} partially) in ",
                    proc.stdout,
                )
                self.assertIn(f"Skipped tests ({case['expected_skip_count']}):", proc.stdout)
                for expected_reason in case["expected_reasons"]:
                    self.assertIn(expected_reason, proc.stdout)
                self.assertIn(f"Partially skipped tests ({case['expected_partial_count']}):", proc.stdout)
                for expected_reason in case["expected_partial_reasons"]:
                    self.assertIn(expected_reason, proc.stdout)

    def test_reports_distinct_partially_skipped_tests(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": False,
                    "stdout": "All tests passed (5 skipped tests, 100 assertions in 10 test cases)\n",
                    "stderr": """
[TEST_EVENT] {"event":"end","name":"test/sql/a.test","status":"ok","passes":4,"fails":0,"skip-mode":3,"partial-skip-reasons":["mode skip flaky planner","mode skip unsupported"]}
[TEST_EVENT] {"event":"end","name":"test/sql/b.test","status":"ok","passes":7,"fails":0,"skip-mode":1,"partial-skip-reasons":["mode skip flaky planner"]}
Skipped tests for the following reasons:
require longdouble: 2
require-env SOME_TOKEN: 3
mode skip flaky planner: 4
mode skip unsupported: 1
""",
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("5 skipped (2 partially)", proc.stdout)
        self.assertIn("Partially skipped tests (2):", proc.stdout)
        partial_summary = proc.stdout.rsplit("Partially skipped tests (2):", 1)[-1]
        self.assertIn("mode skip flaky planner: 2", partial_summary)
        self.assertIn("mode skip unsupported: 1", partial_summary)

    def test_terminal_events_override_aggregate_skip_miscounts(self):
        stdout = "All tests passed (1 skipped test, 100 assertions in 10 test cases)\n"
        stderr = """
[TEST_EVENT] {"event":"end","name":"test/sql/full_a.test","status":"skip-requirement","passes":0,"fails":0,"skip-mode":0,"data":"require windows"}
[TEST_EVENT] {"event":"end","name":"test/sql/full_b.test","status":"skip-requirement","passes":0,"fails":0,"skip-mode":0,"data":"require-env TOKEN"}
[TEST_EVENT] {"event":"end","name":"test/sql/conditional.test","status":"ok","passes":4,"fails":0,"skip-mode":3}
[TEST_EVENT] {"event":"end","name":"test/sql/partial.test","status":"ok","passes":4,"fails":0,"skip-mode":2,"partial-skip-reasons":["mode skip flaky"]}

Skipped tests for the following reasons:
require windows: 2
require-env TOKEN: 3
mode skip flaky: 4
"""

        skipped_count, skipped_reasons, partial_names, partial_reason_names = run_tests.extract_skipped_test_output(
            stdout, stderr
        )

        self.assertEqual(skipped_count, 2)
        self.assertEqual(skipped_reasons, {"require windows": 1, "require-env TOKEN": 1})
        self.assertEqual(partial_names, {"test/sql/partial.test"})
        self.assertEqual(partial_reason_names, {"mode skip flaky": {"test/sql/partial.test"}})

    def test_successful_batches_print_progress_bar(self):
        test_list_path = create_temp_file("test/sql/a.test\ntest/sql/b.test\n")
        result = {
            "failed": False,
            "stdout": "All tests passed (1 assertion in 1 test case)\n",
            "stderr": "",
            "message": None,
            "peak_rss_bytes": 0,
        }

        try:
            with mock.patch("scripts.ci.run_tests.run_batch", side_effect=[result, result]):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(proc.stdout.count(".................................................."), 2)
        self.assertIn("[ 50%]", proc.stdout)
        self.assertIn("[100%]", proc.stdout)

    def test_aggregates_skipped_tests_from_multiple_batches(self):
        test_list_path = create_temp_file("test/sql/a.test\ntest/sql/b.test\n")
        batch_outputs = [
            {
                "failed": False,
                "stdout": """
All tests passed (2 skipped tests, 100 assertions in 1 test cases)
[TEST_EVENT] {"event":"end","name":"test/sql/a.test","status":"ok","passes":4,"fails":0,"skip-mode":2,"partial-skip-reasons":["mode skip flaky planner"]}

Skipped tests for the following reasons:
require windows: 1
require-env A: 1
mode skip flaky planner: 2
""",
                "stderr": "",
                "message": None,
                "peak_rss_bytes": 0,
            },
            {
                "failed": False,
                "stdout": """
All tests passed (3 skipped tests, 100 assertions in 1 test cases)
[TEST_EVENT] {"event":"end","name":"test/sql/b.test","status":"ok","passes":4,"fails":0,"skip-mode":2,"partial-skip-reasons":["mode skip flaky planner","mode skip unsupported"]}

Skipped tests for the following reasons:
require-env A: 2
require-env B: 1
mode skip flaky planner: 1
mode skip unsupported: 1
""",
                "stderr": "",
                "message": None,
                "peak_rss_bytes": 0,
            },
        ]
        try:
            with mock.patch("scripts.ci.run_tests.run_batch", side_effect=batch_outputs):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(", 5 skipped (2 partially) in ", proc.stdout)
        self.assertIn("require windows: 1", proc.stdout)
        self.assertIn("require-env A: 3", proc.stdout)
        self.assertIn("require-env B: 1", proc.stdout)
        self.assertIn("5 skipped (2 partially)", proc.stdout)
        self.assertIn("Partially skipped tests (2):", proc.stdout)
        self.assertIn("mode skip flaky planner: 2", proc.stdout)
        self.assertIn("mode skip unsupported: 1", proc.stdout)

    def test_does_not_double_count_summary_when_in_stdout_and_stderr(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        skip_summary = """
All tests passed (4 skipped tests, 100 assertions in 1 test cases)

Skipped tests for the following reasons:
require json: 3
require-env FOO: 1
"""
        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": False,
                    "stdout": skip_summary,
                    "stderr": skip_summary,
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(", 4 skipped in ", proc.stdout)
        self.assertIn("require json: 3", proc.stdout)
        self.assertIn("require-env FOO: 1", proc.stdout)

    def test_counts_skips_when_summary_is_split_over_stdout_and_stderr(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": False,
                    "stdout": "All tests passed (4 skipped tests, 100 assertions in 1 test cases)\n",
                    "stderr": """
Skipped tests for the following reasons:
require json: 3
require-env FOO: 1
""",
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(", 4 skipped in ", proc.stdout)
        self.assertIn("require json: 3", proc.stdout)
        self.assertIn("require-env FOO: 1", proc.stdout)

    def test_retry_only_counts_skips_from_successful_attempt(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                side_effect=[
                    {
                        "failed": True,
                        "stdout": """
All tests passed (7 skipped tests, 100 assertions in 1 test cases)

Skipped tests for the following reasons:
require windows: 7
""",
                        "stderr": "",
                        "message": "first attempt failed",
                        "peak_rss_bytes": 0,
                    },
                    {
                        "failed": False,
                        "stdout": """
All tests passed (2 skipped tests, 100 assertions in 1 test cases)

Skipped tests for the following reasons:
require windows: 2
""",
                        "stderr": "",
                        "message": None,
                        "peak_rss_bytes": 0,
                    },
                ],
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--retry",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(", 2 skipped in ", proc.stdout)
        summary_block = proc.stdout.rsplit("Skipped tests (2):", 1)[-1]
        self.assertIn("require windows: 2", summary_block)
        self.assertNotIn("require windows: 7", summary_block)

    def test_fail_require_skip_enabled_fails(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        helper_path = create_temp_file(
            """
            #!/bin/sh
            cat <<'EOF' >&2
            Skipped tests for the following reasons:
            require mysql_scanner: 56
            require postgres_scanner: 2
            require spatial: 124
            EOF
            exit 0
            """
        )
        os.chmod(helper_path, 0o755)

        try:
            proc = start_runner(
                [
                    "--workers",
                    "1",
                    "--batch-size",
                    "1",
                    "--fail-require-skip",
                    "--test-list",
                    str(test_list_path),
                    "--test-command",
                    f"{helper_path} {{test_list}}",
                    "unused-binary",
                ]
            )
        finally:
            test_list_path.unlink(missing_ok=True)
            helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("require mysql_scanner: 56", proc.stdout)
        summary_block = proc.stdout.rsplit("Skipped tests (0):", 1)[-1]
        self.assertIn("require mysql_scanner: 56", summary_block)
        self.assertIn("require postgres_scanner: 2", summary_block)
        self.assertIn("require spatial: 124", summary_block)

    def test_generate_list(self):
        listed_tests_path = create_temp_file(
            """
            name\tgroup
            test/sql/slow.test\t[.][slow]
            test/sql/fast.test\t[fast]
            """
        )

        list_helper_path = create_temp_file(
            """
            #!/bin/sh
            # run_tests.py calls: <helper> --list-tests <pattern>
            if [ "$1" != "--list-tests" ]; then
              printf '\\n[0/2] (0%%): test/sql/slow.test\\n'
              printf '[1/2] (50%%): test/sql/slow.test took 0.001s\\n'
              printf '[2/2] (100%%): test/sql/fast.test took 0.002s\\n'
              printf 'All tests passed (100 assertions in 2 test cases)\\n'
              exit 0
            fi
            cat "$2"
            exit 2
            """
        )

        os.chmod(list_helper_path, 0o755)

        try:
            proc = start_runner(
                [
                    "--workers",
                    "1",
                    "--batch-size",
                    "2",
                    "--track-runtime",
                    "0",
                    "--test-command",
                    "{binary} {test_list}",
                    str(list_helper_path),
                    str(listed_tests_path),
                ]
            )
        finally:
            listed_tests_path.unlink(missing_ok=True)
            list_helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("batch_size=2", proc.stdout)
        self.assertNotIn("found 2 tests", proc.stdout)
        self.assertNotIn("forces batch_size=1", proc.stdout)
        self.assertIn("warn: test/sql/slow.test took", proc.stdout)
        self.assertIn("warn: test/sql/fast.test took", proc.stdout)
        self.assertIn("ran tests: ", proc.stdout)

    def test_runs_with_echo_test_command(self):
        test_list_path = create_temp_file("test/sql/a.test\ntest/sql/b.test\n")

        try:
            proc = start_runner(
                [
                    "--workers",
                    "1",
                    "--batch-size",
                    "2",
                    "--test-list",
                    str(test_list_path),
                    "--test-command",
                    "echo fake-run {test_list}",
                    "unused-binary",
                ]
            )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("found 2 tests", proc.stdout)
        self.assertIn("ran tests: ", proc.stdout)
        self.assertNotIn("=== config run: default ===", proc.stdout)
        self.assertNotIn("all 1 config runs passed", proc.stdout)

    def test_changed_tests_flag_uses_second_list_file(self):
        base_test_list_path = create_temp_file(
            """
            test/sql/a.test
            test/sql/b.test
            """
        )
        changed_test_list_path = create_temp_file(
            """
            test/sql/b.test
            test/sql/c.test
            """
        )

        list_helper_path = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" != "--list-tests" ]; then
              exit 2
            fi
            shift
            while [ "$#" -gt 0 ]; do
              if [ "$1" = "-f" ]; then
                shift
                cat "$1"
              fi
              shift
            done
            exit 0
            """
        )

        os.chmod(list_helper_path, 0o755)
        list_commands = []

        real_subprocess_run = run_tests.subprocess.run

        def capture_list_command(*args, **kwargs):
            command = args[0]
            if "--list-tests" in command:
                list_commands.append(command)
            return real_subprocess_run(*args, **kwargs)

        try:
            with mock.patch("scripts.ci.run_tests.subprocess.run", side_effect=capture_list_command):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "2",
                        "--test-list",
                        str(base_test_list_path),
                        "--changed-tests",
                        str(changed_test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        str(list_helper_path),
                    ]
                )
        finally:
            base_test_list_path.unlink(missing_ok=True)
            changed_test_list_path.unlink(missing_ok=True)
            list_helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(list_commands)
        self.assertIn("-f", list_commands[0])
        self.assertIn(str(base_test_list_path), list_commands[0])
        self.assertIn(str(changed_test_list_path), list_commands[0])
        self.assertIn("added 1 tests from --changed-tests file to the smoke test run", proc.stdout)
        self.assertIn("ran tests: ", proc.stdout)

    def test_stabilize_tests_reruns_selected_tests_with_fast_and_slow_policy(self):
        test_list_path = create_temp_file(
            """
            name\tgroup
            test/sql/fast.test\t[fast]
            test/sql/slow.test\t[.][slow]
            """
        )
        run_calls = []

        def fake_run_tests(_config, batches, total_tests):
            run_calls.append({"batches": batches, "total_tests": total_tests})
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=total_tests, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        try:
            with mock.patch("scripts.ci.run_tests.run_tests", side_effect=fake_run_tests):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "10",
                        "--stabilize-tests",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(run_calls), 12)
        self.assertEqual(run_calls[0]["total_tests"], 2)
        fast_reruns = [
            call for call in run_calls[1:] if call["total_tests"] == 1 and call["batches"][0][0].endswith("fast.test")
        ]
        slow_reruns = [
            call for call in run_calls[1:] if call["total_tests"] == 1 and call["batches"][0][0].endswith("slow.test")
        ]
        self.assertEqual(len(fast_reruns), 9)
        self.assertEqual(len(slow_reruns), 2)

    def test_changed_tests_auto_stabilize_reruns_only_added_tests(self):
        base_test_list_path = create_temp_file(
            """
            test/sql/a.test
            """
        )
        changed_test_list_path = create_temp_file(
            """
            test/sql/a.test
            test/sql/b.test
            """
        )
        listed_tests_path = create_temp_file(
            """
            name\tgroup
            test/sql/a.test\t[fast]
            test/sql/b.test\t[fast]
            """
        )
        list_helper_path = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" != "--list-tests" ]; then
              exit 2
            fi
            cat "{listed_tests_path}"
            exit 0
            """,
            listed_tests_path=listed_tests_path,
        )
        os.chmod(list_helper_path, 0o755)
        run_calls = []

        def fake_run_tests(_config, batches, total_tests):
            run_calls.append({"batches": batches, "total_tests": total_tests})
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=total_tests, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        try:
            with mock.patch("scripts.ci.run_tests.run_tests", side_effect=fake_run_tests):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "10",
                        "--test-list",
                        str(base_test_list_path),
                        "--changed-tests",
                        str(changed_test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        str(list_helper_path),
                    ]
                )
        finally:
            base_test_list_path.unlink(missing_ok=True)
            changed_test_list_path.unlink(missing_ok=True)
            listed_tests_path.unlink(missing_ok=True)
            list_helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        rerun_calls = run_calls[1:]
        self.assertEqual(len(rerun_calls), 9)
        for call in rerun_calls:
            self.assertEqual(call["total_tests"], 1)
            self.assertTrue(call["batches"][0][0].endswith("test/sql/b.test"))

    def test_changed_tests_stabilization_rebatches_for_available_workers(self):
        base_names = [f"test/sql/base_{idx}.test" for idx in range(120)]
        changed_names = [f"test/sql/changed_{idx}.test" for idx in range(9)]
        base_test_list_path = create_temp_file("\n".join(base_names) + "\n")
        changed_test_list_path = create_temp_file("\n".join(changed_names) + "\n")
        listed_tests_path = create_temp_file(
            "name\tgroup\n" + "\n".join(f"{name}\t[fast]" for name in base_names + changed_names) + "\n"
        )
        run_calls = []

        def fake_run_tests(_config, batches, total_tests):
            run_calls.append(batches)
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=total_tests, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        try:
            with (
                mock.patch("scripts.ci.run_tests.create_temp_test_list", return_value=listed_tests_path),
                mock.patch("scripts.ci.run_tests.run_tests", side_effect=fake_run_tests),
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "12",
                        "--batch-size",
                        "10",
                        "--test-list",
                        str(base_test_list_path),
                        "--changed-tests",
                        str(changed_test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            base_test_list_path.unlink(missing_ok=True)
            changed_test_list_path.unlink(missing_ok=True)
            listed_tests_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(run_calls), 10)
        self.assertEqual(len(run_calls[0][0]), 10)
        for batches in run_calls[1:]:
            self.assertEqual(batches, [[name] for name in changed_names])

    def test_changed_tests_large_set_uses_fast_three_total_runs(self):
        base_test_list_path = create_temp_file("test/sql/base.test\n")
        changed_lines = ["test/sql/base.test"] + [f"test/sql/new_{idx}.test" for idx in range(501)]
        changed_test_list_path = create_temp_file("\n".join(changed_lines) + "\n")
        listed_rows = (
            ["name\tgroup"]
            + [f"test/sql/new_{idx}.test\t[fast]" for idx in range(501)]
            + ["test/sql/base.test\t[fast]"]
        )
        listed_tests_path = create_temp_file("\n".join(listed_rows) + "\n")
        list_helper_path = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" != "--list-tests" ]; then
              exit 2
            fi
            cat "{listed_tests_path}"
            exit 0
            """,
            listed_tests_path=listed_tests_path,
        )
        os.chmod(list_helper_path, 0o755)
        run_calls = []

        def fake_run_tests(_config, batches, total_tests):
            run_calls.append({"batches": batches, "total_tests": total_tests})
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=total_tests, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        try:
            with mock.patch("scripts.ci.run_tests.run_tests", side_effect=fake_run_tests):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1000",
                        "--test-list",
                        str(base_test_list_path),
                        "--changed-tests",
                        str(changed_test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        str(list_helper_path),
                    ]
                )
        finally:
            base_test_list_path.unlink(missing_ok=True)
            changed_test_list_path.unlink(missing_ok=True)
            listed_tests_path.unlink(missing_ok=True)
            list_helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(run_calls), 3)
        self.assertEqual(run_calls[0]["total_tests"], 502)
        self.assertEqual(run_calls[1]["total_tests"], 501)
        self.assertEqual(run_calls[2]["total_tests"], 501)

    def test_stabilization_failure_fails_config_run(self):
        test_list_path = create_temp_file(
            """
            name\tgroup
            test/sql/fast.test\t[fast]
            """
        )
        run_results = [
            run_tests.ConfigRunResult(
                returncode=0, passed_tests=1, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            ),
            run_tests.ConfigRunResult(
                returncode=1,
                passed_tests=0,
                failed_tests=1,
                skipped_tests=0,
                elapsed_seconds=0.0,
                failed_test_names=("test/sql/fast.test",),
            ),
        ]

        try:
            with mock.patch("scripts.ci.run_tests.run_tests", side_effect=run_results):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "10",
                        "--stabilize-tests",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("error: stabilization rerun failure detected", proc.stdout)
        self.assertIn("reproduce all failed tests:", proc.stdout)
        self.assertIn("run '\"test/sql/fast.test\"'", proc.stdout)

    def test_retries_failed_fake_job(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        state_file_path = create_temp_file("")
        helper_path = create_temp_file(
            """
            #!/bin/sh
            if [ ! -f {state_file_path} ]; then
              touch {state_file_path}
              echo fake failure
              exit 1
            fi
            echo fake success
            exit 0
            """,
            state_file_path=state_file_path,
        )

        os.chmod(helper_path, 0o755)
        state_file_path.unlink(missing_ok=True)

        try:
            proc = start_runner(
                [
                    "--workers",
                    "1",
                    "--batch-size",
                    "1",
                    "--retry",
                    "1",
                    "--max-retries",
                    "4",
                    "--test-list",
                    str(test_list_path),
                    "--test-command",
                    f"{helper_path} {{test_list}}",
                    "unused-binary",
                ]
            )
        finally:
            test_list_path.unlink(missing_ok=True)
            state_file_path.unlink(missing_ok=True)
            helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("retrying failed test test/sql/a.test (attempt 1/1, retry 1/4)", proc.stdout)
        self.assertIn("error: FAIL test/sql/a.test", run_tests.strip_ansi(proc.stdout))
        self.assertIn("fake failure", proc.stdout)
        self.assertIn("recovered: passed on retry 1/1", proc.stdout)
        self.assertEqual(proc.stdout.count("fake failure"), 1)
        self.assertIn("ran tests: ", proc.stdout)
        self.assertNotIn("reproduce all failed tests:", proc.stdout)

    def test_retries_timed_out_sleep_job(self):
        test_list_path = create_temp_file("test/sql/a.test\n")
        batch_timeout = 0.3

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                side_effect=[
                    {
                        "failed": True,
                        "stdout": "",
                        "stderr": "",
                        "message": f"batch timed out after {batch_timeout} seconds",
                        "peak_rss_bytes": 0,
                    },
                    {
                        "failed": False,
                        "stdout": "fake success\n",
                        "stderr": "",
                        "message": None,
                        "peak_rss_bytes": 0,
                    },
                ],
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--retry",
                        "1",
                        "--batch-timeout",
                        str(batch_timeout),
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(f"error: timeout ({batch_timeout}s) for test/sql/a.test.", proc.stdout)
        self.assertIn("recovered: passed on retry 1/1", proc.stdout)
        self.assertIn("retrying failed test test/sql/a.test", proc.stdout)
        self.assertIn("ran tests: ", proc.stdout)

    def test_failed_batch_prints_single_compact_summary_after_retries(self):
        test_list_path = create_temp_file("test/sql/a.test\n")

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                side_effect=[
                    {
                        "failed": True,
                        "stdout": "",
                        "stderr": (
                            "Error: Wrong result in query!\n"
                            "SELECT COUNT(*) FROM integers WHERE i = 1;\n"
                            "Mismatch on row 1, column count_star()(index 1)\n"
                            "16 <> 20001\n"
                        ),
                        "message": None,
                        "peak_rss_bytes": 0,
                    },
                    {
                        "failed": True,
                        "stdout": "",
                        "stderr": (
                            "Error: Wrong result in query!\n"
                            "SELECT COUNT(*) FROM integers WHERE i = 1;\n"
                            "Mismatch on row 1, column count_star()(index 1)\n"
                            "24 <> 20001\n"
                        ),
                        "message": None,
                        "peak_rss_bytes": 0,
                    },
                ],
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--retry",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("error: FAIL test/sql/a.test", run_tests.strip_ansi(proc.stdout))
        self.assertIn("details: Mismatch on row 1, column count_star()(index 1)", proc.stdout)
        self.assertNotIn("### failed test batch", proc.stdout)
        self.assertNotIn("attempts:", proc.stdout)

    def test_final_reproducer_includes_every_failure_in_test_list_order(self):
        test_names = ["test/sql/a.test", "Compiled test name", "test/sql/b.test"]
        test_list_path = create_temp_file("\n".join(test_names) + "\n")
        stdout = """
-------------------------------------------------------------------------------
Compiled test name
-------------------------------------------------------------------------------
/repo/test.cpp:10
...............................................................................

/repo/test.cpp:12: FAILED:
  REQUIRE( false )
with expansion:
  false
"""
        stderr = """
1. test/sql/b.test:7
================================================================================
Error: Catalog Error: b failed
================================================================================
2. test/sql/a.test:9
================================================================================
Error: Catalog Error: a failed
================================================================================
"""

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": True,
                    "stdout": stdout,
                    "stderr": stderr,
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "3",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("reproduce all failed tests:", proc.stdout)
        command = proc.stdout.rstrip().splitlines()[-1]
        self.assertEqual(
            shlex.split(command),
            ["run", '"test/sql/a.test","Compiled test name","test/sql/b.test"'],
        )

    def test_final_reproducer_unions_failures_across_exhausted_retries(self):
        test_list_path = create_temp_file("test/sql/a.test\ntest/sql/b.test\n")

        def failure(test_name):
            return {
                "failed": True,
                "stdout": "",
                "stderr": f"1. {test_name}:4\nError: failed\n",
                "message": None,
                "peak_rss_bytes": 0,
            }

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                side_effect=[failure("test/sql/b.test"), failure("test/sql/a.test")],
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "2",
                        "--retry",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        command = proc.stdout.rstrip().splitlines()[-1]
        self.assertEqual(shlex.split(command), ["run", '"test/sql/a.test","test/sql/b.test"'])

    def test_failed_batch_includes_mismatch_context(self):
        test_list_path = create_temp_file("test/sql/a.test\n")

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": True,
                    "stdout": "",
                    "stderr": (
                        "Error: Wrong result in query!\n"
                        "SELECT COUNT(*) FROM integers WHERE i = 1;\n"
                        "Mismatch on row 1, column count_star()(index 1)\n"
                        "24 <> 20001\n"
                    ),
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("error: FAIL test/sql/a.test", run_tests.strip_ansi(proc.stdout))
        self.assertIn("details: Mismatch on row 1, column count_star()(index 1)", proc.stdout)

    def test_failed_batch_includes_full_expected_and_actual_output(self):
        test_list_path = create_temp_file("test/sql/a.test\n")

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": True,
                    "stdout": "",
                    "stderr": (
                        "Error: Wrong result in query!\n"
                        "SELECT COUNT(*) FROM integers WHERE i = 1;\n"
                        "Mismatch on row 1, column count_star()(index 1)\n"
                        "24 <> 20001\n"
                        "================================================================================\n"
                        "Expected result:\n"
                        "================================================================================\n"
                        "20001\n"
                        "\n"
                        "================================================================================\n"
                        "Actual result:\n"
                        "================================================================================\n"
                        "24\n"
                    ),
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("Expected result:\n20001", proc.stdout)
        self.assertIn("Actual result:\n24", proc.stdout)

    def test_failed_batch_does_not_print_skipped_test_summary_inline(self):
        test_list_path = create_temp_file("test/sql/a.test\n")

        try:
            with mock.patch(
                "scripts.ci.run_tests.run_batch",
                return_value={
                    "failed": True,
                    "stdout": "",
                    "stderr": (
                        "Error: Wrong result in query!\n"
                        "SELECT COUNT(*) FROM integers WHERE i = 1;\n"
                        "Mismatch on row 1, column count_star()(index 1)\n"
                        "24 <> 20001\n"
                        "\n"
                        "Skipped tests for the following reasons:\n"
                        "require windows: 1\n"
                    ),
                    "message": None,
                    "peak_rss_bytes": 0,
                },
            ):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-command",
                        "echo fake-run {test_list}",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        failure_block = proc.stdout.split("reproduce:", 1)[0]
        self.assertNotIn("Skipped tests (", failure_block)
        self.assertNotIn("require windows: 1", failure_block)
        summary_block = proc.stdout.rsplit("Skipped tests (0):", 1)[-1]
        self.assertIn("require windows: 1", summary_block)

    def test_prefers_failing_stderr_block_and_single_test_reproduce(self):
        batch = [
            "/tmp/a.test",
            "/tmp/b.test",
            "test/sql/logging/http_logging.test",
        ]
        stdout = (
            "[2/5] (40%): /tmp/b.test took 0.007s"
            "PRAGMA enable_verification has been deprecated - there is no need to set this anymore\n"
        )
        stderr = """
1. test/sql/logging/http_logging.test:25
================================================================================
Wrong result in query! (test/sql/logging/http_logging.test:25)!
================================================================================
SELECT request.headers['Range'], response.headers['Content-Range']
FROM duckdb_logs_parsed('HTTP')
WHERE request.type='GET';
================================================================================
Mismatch on row 1, column response.headers['Content-Range'](index 2)
NULL <> bytes 0-1275/1276
================================================================================
Expected result:
================================================================================
bytes=0-1275\tbytes 0-1275/1276

================================================================================
Actual result:
================================================================================
bytes=0-1275\tNULL


~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
unittest is a Catch v2.13.7 host application.
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(
            reproduce_batch,
            ["test/sql/logging/http_logging.test"],
        )
        self.assertIn(
            "error: FAIL test/sql/logging/http_logging.test",
            strip_ansi_lines(lines),
        )
        self.assertIn(
            "details: Mismatch on row 1, column response.headers['Content-Range'](index 2)",
            lines,
        )
        self.assertIn("Expected result:", lines)
        self.assertIn("bytes=0-1275\tbytes 0-1275/1276", lines)
        self.assertIn("Actual result:", lines)
        self.assertIn("bytes=0-1275\tNULL", lines)
        self.assertNotIn(
            "PRAGMA enable_verification has been deprecated - there is no need to set this anymore",
            "\n".join(lines),
        )

    def test_generic_failure_uses_failing_stderr_block_instead_of_stdout(self):
        batch = ["/tmp/a.test", "/tmp/fail.test"]
        stdout = """
Filters: /tmp/a.test,/tmp/fail.test
[0/2] (0%): /tmp/a.test
[1/2] (50%): /tmp/a.test took 0.001s
[1/2] (50%): /tmp/fail.test
"""
        stderr = """
1. /tmp/fail.test:7
================================================================================
Error: Catalog Error: nope
================================================================================

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
unittest is a Catch v2.13.7 host application.
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(reproduce_batch, ["/tmp/fail.test"])
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL /tmp/fail.test",
                "",
                "1. /tmp/fail.test:7",
                "================================================================================",
                "Error: Catalog Error: nope",
                "================================================================================",
            ],
        )

    def test_empty_stdout_assertion_uses_sqllogictest_stderr_diagnostics(self):
        batch = ["/tmp/fail.test"]
        stdout = """
Filters: /tmp/fail.test

[0/1] (0%): /tmp/fail.test
-------------------------------------------------------------------------------
/tmp/fail.test
-------------------------------------------------------------------------------
/duckdb/test/sqlite/test_sqllogictest.cpp:47
...............................................................................

/tmp/fail.test:314: FAILED:


[1/1] (100%): /tmp/fail.test took 0.015s
===============================================================================
test cases:  1 |  0 passed | 1 failed
assertions: 28 | 27 passed | 1 failed
"""
        stderr = """
1. /tmp/fail.test:314
================================================================================
Query unexpectedly failed (/tmp/fail.test:314)
 (/tmp/fail.test:314)!
================================================================================
SELECT x'303132' IN (SELECT * FROM t1);
================================================================================
Actual result:
================================================================================
Binder Error: Cannot compare values of type BLOB and INTEGER in IN/ANY/ALL clause - an explicit cast is required

LINE 1: SELECT x'303132' IN (SELECT * FROM t1)
                            ^^^^^^^^^^^^^^^^^^
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        stripped_lines = strip_ansi_lines(lines)

        self.assertEqual(reproduce_batch, batch)
        self.assertIn("error: FAIL /tmp/fail.test", stripped_lines)
        self.assertIn(
            "Binder Error: Cannot compare values of type BLOB and INTEGER in IN/ANY/ALL clause - "
            "an explicit cast is required",
            stripped_lines,
        )
        self.assertIn("LINE 1: SELECT x'303132' IN (SELECT * FROM t1)", stripped_lines)
        self.assertIn("                            ^^^^^^^^^^^^^^^^^^", stripped_lines)

    def test_fatal_stdout_failure_uses_last_started_test_and_signal(self):
        batch = [
            "/duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/slow/hnsw_reclaim_storage.test_slow",
            "/duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/hnsw/hnsw_projection.test",
            "/duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/hnsw/hnsw_lateral_join_group.test",
        ]
        stdout = """
[0/3] (0%): /duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/slow/hnsw_reclaim_storage.test_slow
[1/3] (33%): /duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/hnsw/hnsw_projection.test
[2/3] (66%): /duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/hnsw/hnsw_lateral_join_group.test
/duckdb_build_dir/duckdb/test/sqlite/test_sqllogictest.cpp:41: FAILED:
  {Unknown expression after the reported line}
due to a fatal error condition:
  SIGSEGV - Segmentation violation signal
"""
        stderr = """
Replacing deprecated string __TEST_DIR__ in path "__TEST_DIR__/hnsw_reclaim_space.db" - please replace with {TEST_DIR}
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL /duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/hnsw/hnsw_lateral_join_group.test",
                "",
                "SIGSEGV - Segmentation violation signal",
            ],
        )
        self.assertEqual(
            reproduce_batch,
            ["/duckdb_build_dir/build/release/_deps/vss_extension_fc-src/test/sql/hnsw/hnsw_lateral_join_group.test"],
        )

    def test_fatal_stdout_failure_preserves_stacktrace(self):
        batch = ["test/sql/crash.test"]
        stdout = """
[0/1] (0%): test/sql/crash.test
/duckdb/test/sqlite/test_sqllogictest.cpp:41: FAILED:
  {Unknown expression after the reported line}
due to a fatal error condition:
  SIGSEGV - Segmentation violation signal

/duckdb/build/release/test/unittest(duckdb::CrashHere()+0x12) [0x1234]
/duckdb/build/release/src/libduckdb.so(duckdb::Execute()+0x34) [0x5678]
===============================================================================
test cases: 1 | 1 failed
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, "", batch, returncode=-11)
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL test/sql/crash.test",
                "",
                "SIGSEGV - Segmentation violation signal",
                "",
                "/duckdb/build/release/test/unittest(duckdb::CrashHere()+0x12) [0x1234]",
                "/duckdb/build/release/src/libduckdb.so(duckdb::Execute()+0x34) [0x5678]",
            ],
        )
        self.assertEqual(reproduce_batch, batch)

    def test_signal_only_failure_prefers_returncode_over_unrelated_stdout(self):
        batch = ["test/sql/crash.test"]
        lines, reproduce_batch = run_tests.summarize_failure_output(
            None,
            "before abort\n",
            "",
            batch,
            returncode=-6,
        )
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL test/sql/crash.test",
                "",
                run_tests.format_signal_summary(-6),
            ],
        )
        self.assertEqual(reproduce_batch, ["test/sql/crash.test"])

    def test_signal_only_failure_uses_last_started_test_for_reproduce(self):
        batch = [
            "/tmp/first.test",
            "/tmp/second.test",
            "/tmp/third.test",
        ]
        stdout = """
[0/3] (0%): /tmp/first.test
[1/3] (33%): /tmp/first.test took 0.001s
[1/3] (33%): /tmp/second.test
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(
            None,
            stdout,
            "",
            batch,
            returncode=-11,
        )
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL /tmp/second.test",
                "",
                run_tests.format_signal_summary(-11),
            ],
        )
        self.assertEqual(reproduce_batch, ["/tmp/second.test"])

    def test_sanitizer_output_is_preferred_over_signal_summary(self):
        batch = ["test/sql/asan.test"]
        stderr = """
==123==ERROR: AddressSanitizer: heap-use-after-free on address 0xdeadbeef
READ of size 4 at 0xdeadbeef thread T0
    #0 0x123 in some_frame
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(
            None,
            "",
            stderr,
            batch,
            returncode=-6,
        )
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL test/sql/asan.test",
                "",
                "==123==ERROR: AddressSanitizer: heap-use-after-free on address 0xdeadbeef",
                "READ of size 4 at 0xdeadbeef thread T0",
                "#0 0x123 in some_frame",
            ],
        )
        self.assertEqual(reproduce_batch, ["test/sql/asan.test"])
        self.assertNotIn(run_tests.format_signal_summary(-6), lines)

    def test_thread_sanitizer_output_is_not_truncated(self):
        batch = ["test/sql/threadsan.test"]
        stderr = """
WARNING: ThreadSanitizer: data race (pid=123)
  Read of size 8 by thread T2:
    #0 duckdb::ArenaAllocator::AlignNext()

  Previous write of size 8 by thread T1:
    #0 duckdb_tdigest::TDigest::updateCumulative()

  Location is heap block allocated by main thread:
    #0 operator new(unsigned long)

SUMMARY: ThreadSanitizer: data race in duckdb::ArenaAllocator::AlignNext()
==================
ThreadSanitizer: reported 1 warnings
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, "", stderr, batch, returncode=66)
        stripped_lines = strip_ansi_lines(lines)

        self.assertIn("WARNING: ThreadSanitizer: data race (pid=123)", stripped_lines)
        self.assertIn("Previous write of size 8 by thread T1:", stripped_lines)
        self.assertIn("Location is heap block allocated by main thread:", stripped_lines)
        self.assertIn("SUMMARY: ThreadSanitizer: data race in duckdb::ArenaAllocator::AlignNext()", stripped_lines)
        self.assertIn("ThreadSanitizer: reported 1 warnings", stripped_lines)
        self.assertEqual(reproduce_batch, batch)

    def test_stdout_failed_block_extracts_explicit_message_reason(self):
        batch = ["/tmp/a.test", "/tmp/fail.test"]
        stdout = """
[0/2] (0%): /tmp/a.test
[1/2] (50%): /tmp/fail.test
/tmp/fail.test:25: FAILED:
explicitly with message:
  catalog blew up
"""
        stderr = "unrelated warning from another test\n"
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL /tmp/fail.test",
                "",
                "catalog blew up",
            ],
        )
        self.assertEqual(reproduce_batch, ["/tmp/fail.test"])

    def test_stdout_failed_block_prefers_full_catch_assertion_block(self):
        progress_bar_path = REPO_ROOT / "test" / "api" / "test_progress_bar.cpp"
        batch = [
            "Pending Query with Parameters",
            "Pending Query with Parameters Catalog Error",
            "Pending Query with Parameters Type Conversion Error",
            "Pending Query with Parameters with transactions",
            "Test Progress Bar Fast",
            "Test UUID API",
            "Test Bignum::FromByteArray",
            "Test deadlock issue between NumberOfThreads and RelaunchThreads",
            "Test database maximum_threads argument",
            "Test external threads",
        ]
        stdout = """
[4/10] (40%): Pending Query with Parameters with transactions took 0.188s
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
unittest is a Catch v2.13.7 host application.
Run with -? for options

-------------------------------------------------------------------------------
Test Progress Bar Fast
-------------------------------------------------------------------------------
{progress_bar_path}:102
...............................................................................

{progress_bar_path}:84: FAILED:
  REQUIRE( cur_rows_read == total_cardinality )
with expansion:
  20000 (0x4e20) == 100020002 (0x5f62f22)

[10/10] (100%): Test external threads took 0.079s
===============================================================================
test cases:  10 |   9 passed | 1 failed
assertions: 359 | 358 passed | 1 failed
""".format(
            progress_bar_path=progress_bar_path
        )
        stderr = ""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(reproduce_batch, ["Test Progress Bar Fast"])
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL Test Progress Bar Fast",
                "",
                '    81          if (std::getenv("FORCE_ASYNC_SINK_SOURCE") != nullptr) {',
                "    82              return;",
                "    83          }",
                "  > 84          error.SetError([cur_rows_read, total_cardinality]() { REQUIRE(cur_rows_read == total_cardinality); });",
                "    85      }",
                "    86  }",
                "    87  void Start() {",
                "",
                "FAILED: REQUIRE( cur_rows_read == total_cardinality )",
                "  with expansion:",
                "20000 (0x4e20) == 100020002 (0x5f62f22)",
            ],
        )

    def test_msvc_catch_assertion_location_is_parsed(self):
        # MSVC builds of Catch print source locations as "path(line):" instead of "path:line:"
        progress_bar_path = REPO_ROOT / "test" / "api" / "test_progress_bar.cpp"
        batch = [
            "Test Progress Bar Fast",
            "Test UUID API",
        ]
        stdout = """
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
unittest is a Catch v2.13.7 host application.
Run with -? for options

-------------------------------------------------------------------------------
Test Progress Bar Fast
-------------------------------------------------------------------------------
{progress_bar_path}(102)
...............................................................................

{progress_bar_path}(84): FAILED:
  REQUIRE( cur_rows_read == total_cardinality )
with expansion:
  20000 (0x4e20) == 100020002 (0x5f62f22)

[2/2] (100%): Test UUID API took 0.079s
===============================================================================
test cases:  2 |   1 passed | 1 failed
assertions: 359 | 358 passed | 1 failed
""".format(
            progress_bar_path=progress_bar_path
        )
        stderr = ""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(reproduce_batch, ["Test Progress Bar Fast"])
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL Test Progress Bar Fast",
                "",
                '    81          if (std::getenv("FORCE_ASYNC_SINK_SOURCE") != nullptr) {',
                "    82              return;",
                "    83          }",
                "  > 84          error.SetError([cur_rows_read, total_cardinality]() { REQUIRE(cur_rows_read == total_cardinality); });",
                "    85      }",
                "    86  }",
                "    87  void Start() {",
                "",
                "FAILED: REQUIRE( cur_rows_read == total_cardinality )",
                "  with expansion:",
                "20000 (0x4e20) == 100020002 (0x5f62f22)",
            ],
        )

    def test_unparsable_batch_failure_dumps_raw_output(self):
        # when we cannot extract a useful failure description, the raw unittest output should be dumped
        # so that CI logs contain everything needed to diagnose the failure
        batch = ["/tmp/a.test", "/tmp/b.test"]
        stdout = """
[0/2] (0%): /tmp/a.test
[TEST_EVENT] {"event":"begin","name":"/tmp/a.test"}
[1/2] (50%): /tmp/a.test took 0.1s
[1/2] (50%): /tmp/b.test
===============================================================================
test cases:  2 |   1 passed | 1 failed
assertions: 16 | 15 passed | 1 failed
"""
        stderr = ""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch, returncode=1)
        stripped_lines = strip_ansi_lines(lines)
        self.assertIn("--- raw unittest stdout ---", stripped_lines)
        self.assertIn("--- raw unittest stderr: empty ---", stripped_lines)
        self.assertIn("assertions: 16 | 15 passed | 1 failed", stripped_lines)
        self.assertNotIn("[TEST_EVENT]", "\n".join(stripped_lines))
        self.assertEqual(reproduce_batch, ["/tmp/a.test", "/tmp/b.test"])

    def test_informative_failures_do_not_dump_raw_output(self):
        batch = ["/tmp/fail.test"]
        stderr = """
================================================================
FAIL: /tmp/fail.test
================================================================
Wrong result in query! (/tmp/fail.test:25)!
================================================================
Mismatch on row 1, column 1
[3, 1, 2] <> [1, 2, 3]
"""
        lines, _ = run_tests.summarize_failure_output(None, "", stderr, batch)
        stripped_lines = strip_ansi_lines(lines)
        self.assertFalse(any(line.startswith("--- raw unittest") for line in stripped_lines))

    def test_generic_failure_merges_query_diagnostics_with_assertion_failure(self):
        remote_optimizer_path = REPO_ROOT / "test" / "extension" / "test_remote_optimizer.cpp"
        batch = ["Test using a remote optimizer pass in case thats important to someone"]
        stdout = """
Filters: Test using a remote optimizer pass in case thats important to someone
[0/1] (0%): Test using a remote optimizer pass in case thats important to someoneFailed to bind socket in child process: Operation not permitted
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
unittest is a Catch v2.13.7 host application.
Run with -? for options

-------------------------------------------------------------------------------
Test using a remote optimizer pass in case thats important to someone
-------------------------------------------------------------------------------
{remote_optimizer_path}:29
...............................................................................

{remote_optimizer_path}:148: FAILED:
  REQUIRE( NO_FAIL((con1.Query( "SELECT first_name FROM PARQUET_SCAN('data/parquet-testing/userdata1.parquet') GROUP BY first_name"))) )
with expansion:
  false

[1/1] (100%): Test using a remote optimizer pass in case thats important to someone took 0.166s
===============================================================================
test cases: 1 | 1 failed
assertions: 4 | 3 passed | 1 failed
""".format(
            remote_optimizer_path=remote_optimizer_path
        )
        stderr = """
Query failed with message: INTERNAL Error: Failed to read "8" bytes from socket - read 0 instead

Stack Trace:

0        _ZN6duckdb9Exception6ToJSONENS_13ExceptionTypeERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE + 48
1        _ZN6duckdb17InternalExceptionC1ERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE + 32
2        _ZN15WaggleExtension11ReadCheckedEiPvy + 220

This error signals an assertion failure within DuckDB. This usually occurs due to unexpected conditions or errors in the program's logic.
For more information, see https://duckdb.org/docs/current/dev/internal_errors
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(reproduce_batch, batch)
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL Test using a remote optimizer pass in case thats important to someone",
                "",
                "    145  }",
                "    146  ",
                "    147  REQUIRE_NO_FAIL(con1.Query(",
                "  > 148      \"SELECT first_name FROM PARQUET_SCAN('data/parquet-testing/userdata1.parquet') GROUP BY first_name\"));",
                "    149  ",
                "    150  if (kill(pid, SIGKILL) != 0) {",
                "    151      FAIL();",
                "",
                'Query failed with message: INTERNAL Error: Failed to read "8" bytes from socket - read 0 instead',
                "Stack Trace:",
                "0        _ZN6duckdb9Exception6ToJSONENS_13ExceptionTypeERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE + 48",
                "1        _ZN6duckdb17InternalExceptionC1ERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE + 32",
                "2        _ZN15WaggleExtension11ReadCheckedEiPvy + 220",
                "",
                "FAILED: REQUIRE( NO_FAIL((con1.Query( \"SELECT first_name FROM PARQUET_SCAN('data/parquet-testing/userdata1.parquet') GROUP BY first_name\"))) )",
                "  with expansion:",
                "false",
            ],
        )

    def test_generic_failure_extracts_sqllogictest_assertion_details(self):
        batch = ["test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow"]
        stdout = """
Filters: test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow
[0/1] (0%): test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
unittest is a Catch v2.13.7 host application.
Run with -? for options

-------------------------------------------------------------------------------
test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow
-------------------------------------------------------------------------------
/Users/sander/dev/duckdb/duckdb/test/sqlite/test_sqllogictest.cpp:41
...............................................................................

test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow:27: FAILED:
explicitly with message:
  0

[1/1] (100%): test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow took 1.086s
===============================================================================
test cases: 1 | 1 failed
assertions: 8 | 7 passed | 1 failed
"""
        stderr = """
1. test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow:27
================================================================================

::error::Query unexpectedly failed! (test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow:27)!
================================================================================
COPY (SELECT p, v FROM large_ordered_source ORDER BY v DESC, p DESC)
TO 'duckdb_unittest_tempdir/11276/large_ordered_partitioned_1'
(FORMAT PARQUET, PARTITION_BY (p), ORDER_BY (p, v), ROW_GROUP_SIZE 2048);
================================================================================
INTERNAL Error: Assertion triggered in file "/Users/sander/dev/duckdb/duckdb/src/execution/operator/persistent/physical_copy_to_file.cpp" on line 1335: batch_state.mode == PartitionedCopyBatchMode::PREPARING

Stack Trace:

0        duckdb::Exception::Exception(duckdb::ExceptionType, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) + 288
1        duckdb::InternalException::InternalException<char const*&, int&, char const*&>(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, char const*&, int&, char const*&) + 396
2        duckdb::DuckDBAssertInternal(bool, char const*, char const*, int) + 480
3        duckdb::PartitionedCopyHashGroup::Prepare(duckdb::ExecutionContext&, duckdb::InterruptState&, duckdb::PartitionedCopyTask const&) + 660

This error signals an assertion failure within DuckDB. This usually occurs due to unexpected conditions or errors in the program's logic.
For more information, see https://duckdb.org/docs/current/dev/internal_errors
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, stderr, batch)
        self.assertEqual(reproduce_batch, batch)
        self.assertEqual(
            strip_ansi_lines(lines)[1:],
            [
                "error: FAIL test/sql/copy/partitioned/partitioned_order_by_flush_race.test_slow",
                "",
                "  > 27  statement ok",
                "    28  COPY (SELECT p, v FROM large_ordered_source ORDER BY v DESC, p DESC)",
                "    29  TO '{TEST_DIR}/large_ordered_partitioned_{i}'",
                "    30  (FORMAT PARQUET, PARTITION_BY (p), ORDER_BY (p, v), ROW_GROUP_SIZE 2048);",
                "",
                ""
                'INTERNAL Error: Assertion triggered in file "/Users/sander/dev/duckdb/duckdb/src/execution/operator/persistent/physical_copy_to_file.cpp" on line 1335: batch_state.mode == PartitionedCopyBatchMode::PREPARING',
                "Stack Trace:",
                "0        duckdb::Exception::Exception(duckdb::ExceptionType, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) + 288",
                "1        duckdb::InternalException::InternalException<char const*&, int&, char const*&>(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, char const*&, int&, char const*&) + 396",
                "2        duckdb::DuckDBAssertInternal(bool, char const*, char const*, int) + 480",
                "3        duckdb::PartitionedCopyHashGroup::Prepare(duckdb::ExecutionContext&, duckdb::InterruptState&, duckdb::PartitionedCopyTask const&) + 660",
            ],
        )

    def test_snippet_trims_blank_edges(self):
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", delete=False) as tmp_file:
            tmp_file.write("\nquery I\nSELECT 42\n----\n42\n\n")
            tmp_file.flush()
            snippet_test_path = Path(tmp_file.name)
        try:
            lines = run_tests.render_test_snippet(str(snippet_test_path), 2)
        finally:
            snippet_test_path.unlink(missing_ok=True)

        self.assertEqual(
            strip_ansi_lines(lines),
            [
                "  > 2  query I",
                "    3  SELECT 42",
                "    4  ----",
                "    5  42",
            ],
        )

    def test_snippet_removes_shared_indentation(self):
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", delete=False) as tmp_file:
            tmp_file.write("        if (a) {\n" "            foo();\n" "            bar();\n" "        }\n")
            tmp_file.flush()
            snippet_test_path = Path(tmp_file.name)
        try:
            lines = run_tests.render_test_snippet(str(snippet_test_path), 2)
        finally:
            snippet_test_path.unlink(missing_ok=True)

        self.assertEqual(
            strip_ansi_lines(lines),
            [
                "    1  if (a) {",
                "  > 2      foo();",
                "    3      bar();",
                "    4  }",
            ],
        )

    def test_snippet_ignores_line_number_past_end_of_file(self):
        snippet_test_path = create_temp_file("query I\nSELECT 42\n----\n42\n")
        try:
            self.assertEqual(run_tests.render_test_snippet(str(snippet_test_path), 4000), [])
            self.assertEqual(run_tests.render_context_snippet(str(snippet_test_path), 4000), [])
        finally:
            snippet_test_path.unlink(missing_ok=True)

    def test_assertion_source_line_is_not_applied_to_the_test_file(self):
        # the assertion location points at a C++ source that is not present, so it renders no snippet of its own
        test_path = create_temp_file("query I\nSELECT 42\n----\n42\n")
        stdout = textwrap.dedent(
            f"""
            [1/2] (50%): {test_path}
            -------------------------------------------------------------------------------
            {test_path}
            -------------------------------------------------------------------------------
            /elsewhere/test/sqlite/sqllogic_test_runner.cpp:4000: FAILED:
              REQUIRE( result->success )
            with expansion:
              false
            """
        )
        try:
            lines, reproduce_batch = run_tests.summarize_failure_output(None, stdout, "", [str(test_path)])
        finally:
            test_path.unlink(missing_ok=True)

        self.assertEqual(
            strip_ansi_lines(lines),
            [
                run_tests.FAILURE_MARKER,
                f"error: FAIL {test_path}",
                "",
                "FAILED: REQUIRE( result->success )",
                "  with expansion:",
                "false",
            ],
        )
        self.assertEqual(reproduce_batch, [str(test_path)])

    def test_timeout_names_last_file_in_multi_test_batch(self):
        lines, reproduce_batch = run_tests.summarize_failure_output(
            "batch timed out after 5 seconds",
            "",
            "",
            ["test/sql/a.test", "test/sql/b.test"],
        )
        self.assertEqual(lines, ["error: timeout (5s) for test/sql/b.test."])
        self.assertEqual(reproduce_batch, ["test/sql/b.test"])

    def test_timeout_uses_first_started_but_not_completed_test(self):
        batch = [
            "/tmp/first.test",
            "/tmp/second.test_slow",
            "/tmp/third.test",
        ]
        stdout = """
[0/10] (0%): /tmp/first.test
[1/10] (10%): /tmp/first.test took 5.334s
"""
        lines, reproduce_batch = run_tests.summarize_failure_output(
            "batch timed out after 300 seconds",
            stdout,
            "",
            batch,
        )
        self.assertEqual(
            lines,
            [
                "error: timeout (300s) for /tmp/second.test_slow.",
            ],
        )
        self.assertEqual(reproduce_batch, ["/tmp/second.test_slow"])

    def test_multiple_test_configs_run_independently(self):
        listed_tests_path = create_temp_file(
            """
            name\tgroup
            test/sql/fast.test\t[fast]
            """
        )

        list_helper_path = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" = "--test-config" ] && [ "$2" = "test/configs/a.json" ] && [ "$3" = "--list-tests" ]; then
              echo "name\tgroup"
              echo "test/sql/a.test\t[fast]"
              exit 0
            fi
            if [ "$1" = "--test-config" ] && [ "$2" = "test/configs/b.json" ] && [ "$3" = "--list-tests" ]; then
              echo "name\tgroup"
              echo "test/sql/b.test\t[fast]"
              exit 0
            fi
            exit 2
            """
        )
        os.chmod(list_helper_path, 0o755)

        try:
            proc = start_runner(
                [
                    "--workers",
                    "1",
                    "--batch-size",
                    "1",
                    "--test-command",
                    "echo fake-run {test_list}",
                    "--test-config",
                    "test/configs/a.json",
                    "--test-config",
                    "test/configs/b.json",
                    str(list_helper_path),
                    str(listed_tests_path),
                ]
            )
        finally:
            list_helper_path.unlink(missing_ok=True)
            listed_tests_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("running 2 configs", proc.stdout)
        self.assertEqual(proc.stdout.count("ran tests: "), 2)
        self.assertIn("all 2 config runs passed", proc.stdout)

    def test_multiple_test_configs_run_concurrently(self):
        with tempfile.TemporaryDirectory() as state_dir:
            helper_path = create_temp_file(
                """
                #!/bin/sh
                if [ "$3" = "--list-tests" ]; then
                  printf 'name\\tgroup\\ntest/sql/a.test\\t[fast]\\n'
                  exit 0
                fi

                case "$2" in
                  *a.json) marker=a ;;
                  *b.json) marker=b ;;
                  *) exit 2 ;;
                esac
                touch "{state_dir}/$marker.started"
                attempts=0
                while [ ! -f "{state_dir}/a.started" ] || [ ! -f "{state_dir}/b.started" ]; do
                  attempts=$((attempts + 1))
                  if [ "$attempts" -gt 100 ]; then
                    exit 3
                  fi
                  sleep 0.02
                done
                """,
                state_dir=state_dir,
            )
            os.chmod(helper_path, 0o755)
            try:
                with mock.patch.dict(os.environ, {"CI": ""}, clear=False):
                    proc = start_runner(
                        [
                            "--workers",
                            "2",
                            "--batch-size",
                            "1",
                            "--test-config",
                            "test/configs/a.json",
                            "--test-config",
                            "test/configs/b.json",
                            str(helper_path),
                        ]
                    )
            finally:
                helper_path.unlink(missing_ok=True)

            a_started = (Path(state_dir) / "a.started").exists()
            b_started = (Path(state_dir) / "b.started").exists()

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(a_started)
        self.assertTrue(b_started)

    def test_next_test_config_starts_only_during_previous_tail(self):
        with tempfile.TemporaryDirectory() as state_dir:
            helper_path = create_temp_file(
                """
                #!/bin/sh
                if [ "$3" = "--list-tests" ]; then
                  if [ "$2" = "test/configs/a.json" ]; then
                    printf 'name\\tgroup\\ntest/sql/a1.test\\t[fast]\\ntest/sql/a2.test\\t[fast]\\n'
                    printf 'test/sql/a3.test\\t[fast]\\ntest/sql/a4.test\\t[fast]\\n'
                  else
                    printf 'name\\tgroup\\ntest/sql/b.test\\t[fast]\\n'
                  fi
                  exit 0
                fi

                for batch_file do :; done
                test_name=$(head -n 1 "$batch_file")
                case "$test_name" in
                  *a1.test) touch "{state_dir}/a1.done" ;;
                  *a2.test) touch "{state_dir}/a2.done" ;;
                  *a3.test)
                    touch "{state_dir}/a3.started"
                    sleep 0.05
                    touch "{state_dir}/a3.done"
                    ;;
                  *a4.test)
                    touch "{state_dir}/a4.started"
                    sleep 1
                    touch "{state_dir}/a4.done"
                    ;;
                  *b.test)
                    [ -f "{state_dir}/a1.done" ] || exit 5
                    [ -f "{state_dir}/a2.done" ] || exit 6
                    [ -f "{state_dir}/a4.started" ] || exit 7
                    [ ! -f "{state_dir}/a4.done" ] || exit 8
                    touch "{state_dir}/b.started"
                    ;;
                  *) exit 9 ;;
                esac
                """,
                state_dir=state_dir,
            )
            os.chmod(helper_path, 0o755)
            try:
                with mock.patch.dict(os.environ, {"CI": ""}, clear=False):
                    proc = start_runner(
                        [
                            "--workers",
                            "2",
                            "--batch-size",
                            "1",
                            "--test-config",
                            "test/configs/a.json",
                            "--test-config",
                            "test/configs/b.json",
                            str(helper_path),
                        ]
                    )
            finally:
                helper_path.unlink(missing_ok=True)

            b_started = (Path(state_dir) / "b.started").exists()

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(b_started)

    def test_multiple_test_configs_share_global_worker_limit(self):
        with tempfile.TemporaryDirectory() as state_dir:
            helper_path = create_temp_file(
                """
                #!/bin/sh
                if [ "$3" = "--list-tests" ]; then
                  printf 'name\\tgroup\\ntest/sql/a.test\\t[fast]\\n'
                  exit 0
                fi

                slot=''
                if mkdir "{state_dir}/slot-1" 2>/dev/null; then
                  slot="{state_dir}/slot-1"
                elif mkdir "{state_dir}/slot-2" 2>/dev/null; then
                  slot="{state_dir}/slot-2"
                else
                  exit 4
                fi
                trap 'rmdir "$slot"' EXIT
                sleep 0.1
                """,
                state_dir=state_dir,
            )
            os.chmod(helper_path, 0o755)
            try:
                with mock.patch.dict(os.environ, {"CI": ""}, clear=False):
                    proc = start_runner(
                        [
                            "--workers",
                            "2",
                            "--batch-size",
                            "1",
                            "--test-config",
                            "test/configs/a.json",
                            "--test-config",
                            "test/configs/b.json",
                            "--test-config",
                            "test/configs/c.json",
                            str(helper_path),
                        ]
                    )
            finally:
                helper_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_single_test_config_uses_live_output_path(self):
        test_list_path = create_temp_file("test/sql/a.test\n")

        def fake_run_single_config(*args, **kwargs):
            self.assertEqual(kwargs, {})
            self.assertEqual(len(args), 10)
            print("live config output")
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=1, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        try:
            with mock.patch("scripts.ci.run_tests.run_single_config", side_effect=fake_run_single_config):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-config",
                        "test/configs/a.json",
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("live config output", proc.stdout)
        self.assertNotIn("running 1 configs", proc.stdout)

    def test_default_config_uses_live_output_path(self):
        test_list_path = create_temp_file("test/sql/a.test\n")

        def fake_run_single_config(*args, **kwargs):
            self.assertEqual(kwargs, {})
            self.assertEqual(len(args), 10)
            self.assertFalse(args[9])
            print("live default output")
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=1, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        try:
            with mock.patch("scripts.ci.run_tests.run_single_config", side_effect=fake_run_single_config):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "unused-binary",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("live default output", proc.stdout)
        self.assertNotIn("=== config run: default ===", proc.stdout)

    def test_multiple_test_configs_buffer_output_separately(self):
        seen_executors = []
        seen_outputs = []

        def fake_run_single_config(*args):
            invocation = args[8]
            executor = args[10]
            output = args[11]
            seen_executors.append(executor)
            seen_outputs.append(output)
            print(f"only in {invocation.label}", file=output)
            return run_tests.ConfigRunResult(
                returncode=0, passed_tests=1, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            )

        with (
            mock.patch.dict(os.environ, {"CI": ""}, clear=False),
            mock.patch("scripts.ci.run_tests.run_single_config", side_effect=fake_run_single_config),
        ):
            proc = start_runner(
                [
                    "--workers",
                    "2",
                    "--test-config",
                    "test/configs/a.json",
                    "--test-config",
                    "test/configs/b.json",
                    "unused-binary",
                ]
            )

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len({id(executor) for executor in seen_executors}), 1)
        self.assertEqual(len({id(output) for output in seen_outputs}), 2)
        for label, other_label in (("a", "b"), ("b", "a")):
            header = f"=== config run: test/configs/{label}.json ==="
            other_header = f"=== config run: test/configs/{other_label}.json ==="
            section_start = proc.stdout.index(header)
            other_start = proc.stdout.find(other_header, section_start + len(header))
            section_end = len(proc.stdout) if other_start == -1 else other_start
            section = proc.stdout[section_start:section_end]
            self.assertIn(f"only in test/configs/{label}.json", section)
            self.assertNotIn(f"only in test/configs/{other_label}.json", section)

    def test_multiple_test_configs_aggregate_failure(self):
        failing_helper = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" = "--test-config" ] && [ "$2" = "test/configs/empty.json" ] && [ "$3" = "--list-tests" ]; then
              echo "name\tgroup"
              exit 0
            fi
            if [ "$1" = "--test-config" ] && [ "$3" = "--list-tests" ]; then
              echo "name\tgroup"
              echo "test/sql/a.test\t[fast]"
              exit 0
            fi
            exit 2
            """
        )
        os.chmod(failing_helper, 0o755)
        try:
            proc = start_runner(
                [
                    "--workers",
                    "1",
                    "--batch-size",
                    "1",
                    "--test-command",
                    "echo fake-run {test_list}",
                    "--test-config",
                    "test/configs/pass.json",
                    "--test-config",
                    "test/configs/empty.json",
                    str(failing_helper),
                    "ignored-pattern",
                ]
            )
        finally:
            failing_helper.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("error: no tests selected for config 'test/configs/empty.json'", proc.stdout)
        self.assertIn("error: 1 config runs failed: test/configs/empty.json", proc.stdout)

    def test_final_reproducer_preserves_flags_and_failed_configs(self):
        test_list_path = create_temp_file("test/sql/a.test\ntest/sql/b.test\n")
        run_results = [
            run_tests.ConfigRunResult(
                returncode=0, passed_tests=2, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0
            ),
            run_tests.ConfigRunResult(
                returncode=1,
                passed_tests=1,
                failed_tests=1,
                skipped_tests=0,
                elapsed_seconds=0.0,
                failed_test_names=("test/sql/b.test",),
            ),
        ]

        try:
            with mock.patch("scripts.ci.run_tests.run_tests", side_effect=run_results):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--test-list",
                        str(test_list_path),
                        "--test-flags",
                        "--force-storage --force-reload",
                        "--test-config",
                        "test/configs/pass.json",
                        "--test-config",
                        "test/configs/fail.json",
                        "build/reldebug/test/unittest",
                    ]
                )
        finally:
            test_list_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        command = proc.stdout.rstrip().splitlines()[-1]
        self.assertEqual(
            shlex.split(command),
            [
                "build/reldebug/test/run",
                "--test-flags",
                "--force-storage --force-reload",
                "--test-config",
                "test/configs/fail.json",
                '"test/sql/b.test"',
            ],
        )

    def test_ci_groups_close_when_all_configs_pass(self):
        listed_tests_path = create_temp_file(
            """
            name\tgroup
            test/sql/fast.test\t[fast]
            """
        )
        list_helper_path = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" = "--test-config" ] && [ "$3" = "--list-tests" ]; then
              echo "name\tgroup"
              echo "test/sql/a.test\t[fast]"
              exit 0
            fi
            exit 2
            """
        )
        os.chmod(list_helper_path, 0o755)
        try:
            with mock.patch.dict(os.environ, {"CI": "1"}, clear=False):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-command",
                        "echo fake-run {test_list}",
                        "--test-config",
                        "test/configs/a.json",
                        "--test-config",
                        "test/configs/b.json",
                        str(list_helper_path),
                        str(listed_tests_path),
                    ]
                )
        finally:
            list_helper_path.unlink(missing_ok=True)
            listed_tests_path.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("::group::test config: test/configs/a.json", proc.stdout)
        self.assertIn("::group::test config: test/configs/b.json", proc.stdout)
        self.assertNotIn("=== config run: test/configs/a.json ===", proc.stdout)
        self.assertNotIn("=== config run: test/configs/b.json ===", proc.stdout)
        self.assertEqual(proc.stdout.count("::endgroup::"), 2)
        self.assertGreaterEqual(proc.stdout.count("ran tests: "), 2)

    def test_ci_groups_stay_open_after_first_failed_config(self):
        failing_helper = create_temp_file(
            """
            #!/bin/sh
            if [ "$1" = "--test-config" ] && [ "$2" = "test/configs/fail.json" ] && [ "$3" = "--list-tests" ]; then
              exit 1
            fi
            if [ "$1" = "--test-config" ] && [ "$3" = "--list-tests" ]; then
              echo "name\tgroup"
              echo "test/sql/a.test\t[fast]"
              exit 0
            fi
            exit 2
            """
        )
        os.chmod(failing_helper, 0o755)
        try:
            with mock.patch.dict(os.environ, {"CI": "1"}, clear=False):
                proc = start_runner(
                    [
                        "--workers",
                        "1",
                        "--batch-size",
                        "1",
                        "--test-command",
                        "echo fake-run {test_list}",
                        "--test-config",
                        "test/configs/fail.json",
                        "--test-config",
                        "test/configs/pass.json",
                        str(failing_helper),
                        "ignored-pattern",
                    ]
                )
        finally:
            failing_helper.unlink(missing_ok=True)

        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("::group::test config: test/configs/fail.json", proc.stdout)
        self.assertIn("::group::test config: test/configs/pass.json", proc.stdout)
        self.assertEqual(proc.stdout.count("::endgroup::"), 2)
        self.assertIn("❌ ran tests: ", proc.stdout)


if __name__ == "__main__":
    unittest.main()
