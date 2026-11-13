#!/usr/bin/env python3
import argparse
import concurrent.futures
import contextlib
import json
import os
import re
import shutil
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import asdict, dataclass
from io import StringIO
from pathlib import Path

DEFAULT_BATCH_SIZE = 10
DEFAULT_BATCH_TIMEOUT_SECONDS = 600
HIGH_WORKER_BATCH_TIMEOUT_SECONDS = 300
HIGH_WORKER_BATCH_TIMEOUT_THRESHOLD = 10
DEFAULT_RSS_MEMORY_THRESHOLD_MIB = 1024
DEFAULT_RUNTIME_THRESHOLD_SECONDS = 10
DEFAULT_RSS_POLL_INTERVAL_SECONDS = 0.05
# Leave some CPU headroom so parallel test execution does not fully saturate CI runners.
DEFAULT_WORKERS = "75%"
DEFAULT_MAX_RETRIES = 4
STABILIZE_SLOW_TOTAL_RUNS = 3
STABILIZE_FAST_TOTAL_RUNS = 10
STABILIZE_FAST_TOTAL_RUNS_LARGE = 3
STABILIZE_CHANGED_TEST_THRESHOLD = 500
STOP_REQUESTED = threading.Event()
ANSI_RED = "\033[31m"
ANSI_TEAL = "\033[36m"
ANSI_DARK_GRAY = "\033[90m"
ANSI_RESET = "\033[0m"
FAILURE_MARKER = "================================================================"


def enable_line_buffering():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True, write_through=True, errors="backslashreplace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(line_buffering=True, write_through=True, errors="backslashreplace")


def signal_stop_requested(signum, frame):
    STOP_REQUESTED.set()


def stop_requested():
    return STOP_REQUESTED.is_set()


@dataclass(frozen=True)
class TestRunnerConfig:
    test_list: Path
    unittest_bin: str
    test_flags: str
    patterns: list[str]
    test_command: str
    workers: int
    retry: int
    max_retries: int
    batch_size: int
    batch_timeout_seconds: float
    rss_memory_threshold_mib: int | None
    runtime_threshold_seconds: int | None
    max_failures: int | None
    fail_require_skip: bool
    coverage_profile_dir: Path | None
    timeout_policy: str = "fail"


@dataclass(frozen=True)
class TestCase:
    name: str
    is_slow: bool


@dataclass(frozen=True)
class FailedAttempt:
    lines: list[str]
    reproduce_batch: list[str]
    failed_test_names: list[str]


@dataclass(frozen=True)
class FailureInfo:
    kind: str
    test_name: str | None
    line_number: int | None
    mismatch_context: str | None
    expected: str | None
    actual: str | None
    snippet_lines: list[str]
    expected_result_lines: list[str]
    actual_result_lines: list[str]
    detail_lines: list[str]
    timeout_seconds: float | None
    reproduce_batch: list[str]


@dataclass(frozen=True)
class StdoutAssertionFailure:
    test_name: str | None
    line_number: int | None
    snippet_lines: list[str]
    detail_lines: list[str]
    score: int


@dataclass(frozen=True)
class StdoutParse:
    last_started_test: str | None
    last_unfinished_test: str | None
    preferred_assertion: StdoutAssertionFailure | None
    fallback_failure_block: tuple[str | None, list[str]] | None
    fatal_error_lines: list[str]
    failed_reason_line: str | None


@dataclass(frozen=True)
class StderrParse:
    test_name: str | None
    line_number: int | None
    failing_summary_block: list[str]
    wrong_result_line: str | None
    mismatch_context: str | None
    mismatch_line: str | None
    expected_result_lines: list[str]
    actual_result_lines: list[str]
    query_failure_lines: list[str]
    first_error_line: str | None


class BatchRunState:
    def __init__(self):
        self.failed_count = 0
        self.retry_count = 0
        self.stop_launching = False
        self.failed_attempts = {}
        self.failed_test_names = set()
        self.timed_out_test_names = []

    def record_retry(self):
        self.retry_count += 1

    def record_failure(self):
        self.failed_count += 1

    def add_failed_attempt(
        self, batch_idx: int, lines: list[str], reproduce_batch: list[str], failed_test_names: list[str]
    ):
        self.failed_attempts.setdefault(batch_idx, []).append(
            FailedAttempt(lines=lines, reproduce_batch=reproduce_batch, failed_test_names=failed_test_names)
        )

    def record_failed_test_names(self, attempts: list[FailedAttempt]):
        for attempt in attempts:
            self.failed_test_names.update(attempt.failed_test_names)

    def pop_failed_attempts(self, batch_idx: int):
        return self.failed_attempts.pop(batch_idx, [])

    def can_retry(self, batch_info, config: TestRunnerConfig):
        return batch_info["attempt"] < config.retry and self.retry_count < config.max_retries

    def should_stop(self, config: TestRunnerConfig):
        return self.stop_launching or config.max_failures is not None and self.failed_count >= config.max_failures


class DotProgressBar:
    def __init__(self, total_batches: int, output=None):
        self.total_batches = total_batches
        self.printed_dots = 0
        self.row_width = 50
        self._line_open = False
        self.output = output if output is not None else sys.stdout

    def _write(self, text: str):
        self.output.write(text)
        self.output.flush()

    def flush_line(self):
        if self._line_open:
            self._write("\n")
            self._line_open = False

    def print_message(self, text: str):
        self.flush_line()
        print(text, file=self.output)

    def advance(self, completed_batches: int):
        if self.total_batches <= 0:
            return
        target_dots = min(100, int((completed_batches * 100) / self.total_batches))
        while self.printed_dots < target_dots:
            self._write(".")
            self.printed_dots += 1
            self._line_open = True
            if self.printed_dots % self.row_width == 0:
                self._write(" [{:3}%]\n".format(self.printed_dots))
                self._line_open = False


@dataclass
class RunContext:
    executor: concurrent.futures.ThreadPoolExecutor
    config: TestRunnerConfig
    state: BatchRunState
    future_to_batch: dict
    progress: DotProgressBar


def chunked(items, n):
    # Keep input order, cap batches at n entries, and isolate slow tests so
    # each batch contains at most one slow test.
    batch = []
    slow_count = 0
    for item in items:
        if batch and (len(batch) >= n or (item.is_slow and slow_count >= 1)):
            yield batch
            batch = []
            slow_count = 0
        batch.append(item.name)
        if item.is_slow:
            slow_count += 1
    if batch:
        yield batch


def compute_batch_size(test_count: int, config: TestRunnerConfig):
    if test_count == 0:
        return 1
    return max(1, min(config.batch_size, (test_count + config.workers - 1) // config.workers))


def split_fast_slow_tests(tests: list[TestCase]):
    fast_tests = [test for test in tests if not test.is_slow]
    slow_tests = [test for test in tests if test.is_slow]
    return fast_tests, slow_tests


def stabilization_extra_runs(candidate_count: int):
    fast_total_runs = STABILIZE_FAST_TOTAL_RUNS
    if candidate_count > STABILIZE_CHANGED_TEST_THRESHOLD:
        fast_total_runs = STABILIZE_FAST_TOTAL_RUNS_LARGE
    slow_total_runs = STABILIZE_SLOW_TOTAL_RUNS
    return max(0, fast_total_runs - 1), max(0, slow_total_runs - 1)


def load_tests(path: Path):
    tests = []
    with path.open("r", encoding="utf8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue

            # Skip header row from `--list-tests`.
            if line == "name\tgroup":
                continue

            if "\t" not in line:
                name = line
                is_slow = line.endswith(".test_slow")
            else:
                columns = line.split("\t")
                assert len(columns) == 2, repr(columns)

                name, group = columns
                is_slow = "[.]" in group
                assert not name.endswith(".test_slow") or is_slow, name

            tests.append(TestCase(name=name, is_slow=is_slow))

    return tests


def build_test_command(config: TestRunnerConfig, test_list: str):
    test_flags = shlex.split(config.test_flags)
    if "--emit-test-events" not in test_flags:
        test_flags.append("--emit-test-events")
    flags = shlex.join(test_flags)
    return config.test_command.format(
        binary=shlex.quote(config.unittest_bin),
        flags=flags,
        test_list=test_list,
    )


def get_process_rss_bytes(pid: int):
    if sys.platform.startswith("linux"):
        try:
            with open(f"/proc/{pid}/status", "r", encoding="utf8") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        fields = line.split()
                        return int(fields[1]) * 1024
        except (FileNotFoundError, ProcessLookupError, ValueError):
            return None
        return None

    if sys.platform == "darwin":
        try:
            proc = subprocess.run(
                ["ps", "-o", "rss=", "-p", str(pid)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                encoding="utf8",
                errors="backslashreplace",
            )
        except OSError:
            return None

        if proc.returncode != 0:
            return None

        output = proc.stdout.strip()
        if not output:
            return None

        try:
            return int(output) * 1024
        except ValueError:
            return None

    return None


def format_mib(value_bytes: int):
    return value_bytes / (1024 * 1024)


def resolve_workers(workers: str):
    cpu_count = os.cpu_count() or 1
    workers = workers.strip()
    if workers.endswith("%"):
        percentage = int(workers[:-1])
        return max(1, int(cpu_count * (percentage / 100.0)))
    return max(1, int(workers))


def resolve_batch_timeout(batch_timeout: float | None, workers: int):
    if batch_timeout is not None:
        return batch_timeout
    if workers >= HIGH_WORKER_BATCH_TIMEOUT_THRESHOLD:
        return HIGH_WORKER_BATCH_TIMEOUT_SECONDS
    return DEFAULT_BATCH_TIMEOUT_SECONDS


def generate_test_list(
    test_file,
    unittest_bin: str,
    test_flags: str,
    patterns: list[str],
    test_list_files: list[Path] | None = None,
    coverage_profile_dir: Path | None = None,
    error_output=None,
):
    # Catch can return a non-zero status code for list commands when tests
    # are found, so we accept non-zero if stdout still contains test output.
    list_file_args = []
    if test_list_files:
        list_file_args = [arg for test_list_file in test_list_files for arg in ("-f", str(test_list_file))]
    command = [unittest_bin, *shlex.split(test_flags), "--list-tests", *list_file_args, *patterns]
    child_env = os.environ.copy()
    if coverage_profile_dir is not None:
        coverage_profile_dir.mkdir(parents=True, exist_ok=True)
        child_env["LLVM_PROFILE_FILE"] = coverage_profile_file_pattern(coverage_profile_dir)
    proc = subprocess.run(
        command,
        text=True,
        encoding="utf8",
        errors="backslashreplace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=child_env,
    )
    if proc.returncode != 0 and not proc.stdout:
        print(
            "Stderr:",
            proc.stderr,
            end="",
            file=error_output if error_output is not None else sys.stderr,
            flush=True,
        )
        raise RuntimeError(f"failed to generate test list from {unittest_bin} (exit: {proc.returncode})")
    test_file.write(proc.stdout)
    test_file.flush()


@contextlib.contextmanager
def open_test_list(
    test_list: Path | None,
    unittest_bin: str,
    test_flags: str,
    patterns: list[str],
    test_list_files: list[Path] | None = None,
    coverage_profile_dir: Path | None = None,
):
    if test_list is not None and (test_list_files is None or len(test_list_files) == 1):
        yield test_list
        return

    with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", delete=False) as test_file:
        generate_test_list(test_file, unittest_bin, test_flags, patterns, test_list_files, coverage_profile_dir)
    result = Path(test_file.name)
    yield result
    result.unlink()


def format_duration_seconds(value: float):
    if value.is_integer():
        return str(int(value))
    return f"{value:g}"


def format_fail_header(test_name: str):
    return [FAILURE_MARKER, f"error: {ANSI_RED}FAIL{ANSI_RESET} {test_name}"]


def format_dark_gray(text: str):
    return f"{ANSI_DARK_GRAY}{text}{ANSI_RESET}"


def highlight_stack_frame_line(line: str):
    match = re.match(r"^(\d+)(\s+)(.+)$", line)
    if not match:
        return line

    frame_idx, spacing, remainder = match.groups()
    delimiters = [pos for pos in (remainder.find("("), remainder.find("<")) if pos != -1]
    if delimiters:
        split_idx = min(delimiters)
        function_name = remainder[:split_idx]
        suffix = remainder[split_idx:]
    else:
        function_name = remainder
        suffix = ""

    return f"{format_dark_gray(frame_idx)}{spacing}{ANSI_TEAL}{function_name}{ANSI_RESET}{suffix}"


def should_filter_stack_frame_line(line: str):
    match = re.match(r"^\d+\s+(.+)$", line)
    if not match:
        return False
    remainder = match.group(1)
    for marker in (
        "invokeActiveTestCase",
        "Catch::RunContext",
        "Catch::Session::run",
        "Catch::Session::runInternal",
        "main(",
    ):
        if marker in remainder:
            return True
    return False


def highlight_stack_trace_lines(lines: list[str]):
    highlighted = []
    in_stack_trace = False
    for line in lines:
        if line == "Stack Trace:":
            in_stack_trace = True
            highlighted.append(line)
            continue
        if in_stack_trace and re.match(r"^\d+\s+", line):
            if should_filter_stack_frame_line(line):
                continue
            highlighted.append(highlight_stack_frame_line(line))
            continue
        if in_stack_trace:
            in_stack_trace = False
        highlighted.append(line)
    return highlighted


def render_test_snippet(test_name: str | None, line_number: int | None):
    if not test_name or line_number is None or line_number <= 0:
        return []
    test_path = Path(test_name)
    if not test_path.exists():
        return []
    try:
        file_lines = test_path.read_text(encoding="utf8").splitlines()
    except OSError:
        return []

    if line_number > len(file_lines):
        return []
    start_idx = line_number - 1
    while start_idx > 0 and file_lines[start_idx - 1].strip():
        start_idx -= 1
    return render_snippet_window(file_lines, line_number, start_idx, None)


def render_snippet_window(file_lines: list[str], line_number: int, start_idx: int, end_idx: int | None):
    if end_idx is None:
        end_idx = min(line_number, len(file_lines))
        while end_idx < len(file_lines) and file_lines[end_idx].strip():
            end_idx += 1
    start_idx = max(0, start_idx)
    end_idx = min(end_idx, len(file_lines))
    window = [(idx + 1, file_lines[idx]) for idx in range(start_idx, end_idx)]
    while window and not window[0][1].strip():
        window.pop(0)
    while window and not window[-1][1].strip():
        window.pop()
    if not window:
        return []

    common_indent = None
    for _, text in window:
        if not text.strip():
            continue
        indent_len = len(text) - len(text.lstrip(" \t"))
        indent = text[:indent_len]
        if common_indent is None:
            common_indent = indent
            continue
        shared_len = 0
        max_len = min(len(common_indent), len(indent))
        while shared_len < max_len and common_indent[shared_len] == indent[shared_len]:
            shared_len += 1
        common_indent = common_indent[:shared_len]
    if common_indent:
        window = [(lineno, text[len(common_indent) :] if text.strip() else text) for lineno, text in window]
    window = [(lineno, text.expandtabs(4)) for lineno, text in window]

    width = len(str(window[-1][0]))
    rendered = []
    for lineno, text in window:
        marker = f"{ANSI_RED}>{ANSI_RESET}" if lineno == line_number else " "
        line_number_text = f"{ANSI_DARK_GRAY}{lineno:>{width}}{ANSI_RESET}"
        rendered.append(f"  {marker} {line_number_text}  {text}")
    return rendered


def render_context_snippet(test_name: str | None, line_number: int | None, before: int = 3, after: int = 3):
    if not test_name or line_number is None or line_number <= 0:
        return []
    test_path = Path(test_name)
    if not test_path.exists():
        return []
    try:
        file_lines = test_path.read_text(encoding="utf8").splitlines()
    except OSError:
        return []

    start_idx = max(0, line_number - 1 - before)
    end_idx = min(len(file_lines), line_number + after)
    return render_snippet_window(file_lines, line_number, start_idx, end_idx)


FAILING_TEST_PATTERN = re.compile(r"^\d+\.\s+(.+?):(\d+)$")
ERROR_LINE_LOCATION_PATTERN = re.compile(r"\((.+?):(\d+)\)!?$")
WRONG_RESULT_PATTERN = re.compile(r"^(?:Error:\s+)?Wrong result in query!\s*(?:\((.+?):(\d+)\)!)?$")
PROGRESS_TEST_START_PATTERN = re.compile(r"^\[\d+/\d+\] \(\d+%\): (.+)$")
FATAL_ERROR_PATTERN = re.compile(r"^\s*due to a fatal error condition:\s*$")
FAILED_HEADER_PATTERN = re.compile(r"^\s*.+:\s+FAILED:\s*$")
EXPLICIT_MESSAGE_PATTERN = re.compile(r"^\s*explicitly with message:\s*$")
# Catch prints source locations as "path:line:" with GCC/Clang and as "path(line):" with MSVC
CATCH_ASSERTION_LOCATION_PATTERN = re.compile(r"^(.+?)(?::(\d+)|\((\d+)\)): FAILED:$")
SANITIZER_PATTERN = re.compile(
    r"(AddressSanitizer|LeakSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer)", flags=re.IGNORECASE
)
SANITIZER_OR_ASSERT_PATTERN = re.compile(
    r"(AddressSanitizer|LeakSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|runtime error:|assert)",
    flags=re.IGNORECASE,
)


def find_failing_test(stderr_lines: list[str], batch):
    batch_test_name = batch[0] if len(batch) == 1 else None
    test_name = batch_test_name
    line_number = None
    for line in stderr_lines:
        match = FAILING_TEST_PATTERN.match(line.strip())
        if match:
            test_name = match.group(1)
            line_number = int(match.group(2))
            break
    return test_name, line_number


def parse_stdout_progress(stdout_lines: list[str]):
    started_tests = []
    completed_tests = set()

    for line in stdout_lines:
        stripped = line.strip()
        progress_match = PROGRESS_TEST_START_PATTERN.match(stripped)
        if not progress_match:
            continue
        progress_text = progress_match.group(1)
        if " took " in progress_text:
            completed_tests.add(progress_text.split(" took ", 1)[0])
        else:
            started_tests.append(progress_text)

    return started_tests, completed_tests


def infer_timed_out_test_from_stdout(stdout_lines: list[str], batch):
    started_tests, completed_tests = parse_stdout_progress(stdout_lines)

    for test_name in started_tests:
        if test_name not in completed_tests:
            return test_name
    if completed_tests and batch:
        for test_name in batch:
            if test_name not in completed_tests:
                return test_name
    if started_tests:
        return started_tests[-1]
    if batch:
        return batch[-1]
    return None


def extract_fatal_error_lines(stdout_lines: list[str]):
    for idx, line in enumerate(stdout_lines):
        if not FATAL_ERROR_PATTERN.match(line):
            continue

        lines = []
        for next_line in stdout_lines[idx + 1 :]:
            stripped = next_line.strip()
            if PROGRESS_TEST_START_PATTERN.match(stripped):
                break
            if stripped.startswith("test cases:") or stripped.startswith("assertions:"):
                break
            if stripped.startswith("==============================================================================="):
                break
            if stripped.startswith("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"):
                break
            lines.append(stripped)

        while lines and not lines[0].strip():
            lines.pop(0)
        while lines and not lines[-1].strip():
            lines.pop()
        return lines
    return []


def extract_failed_reason_line(stdout_lines: list[str]):
    for idx, line in enumerate(stdout_lines):
        if not FAILED_HEADER_PATTERN.match(line):
            continue

        for lookahead_idx in range(idx + 1, len(stdout_lines)):
            stripped = stdout_lines[lookahead_idx].strip()
            if not stripped:
                continue
            if FATAL_ERROR_PATTERN.match(stdout_lines[lookahead_idx]):
                for next_line in stdout_lines[lookahead_idx + 1 :]:
                    next_stripped = next_line.strip()
                    if next_stripped:
                        return next_stripped
                return None
            if EXPLICIT_MESSAGE_PATTERN.match(stdout_lines[lookahead_idx]):
                for next_line in stdout_lines[lookahead_idx + 1 :]:
                    next_stripped = next_line.strip()
                    if next_stripped:
                        return next_stripped
                return None
            if stripped.startswith("{") and stripped.endswith("}"):
                continue
            return stripped
    return None


def find_catch_failure_test_name(stdout_lines: list[str], failure_idx: int):
    separator_indices = []
    for idx in range(failure_idx - 1, -1, -1):
        stripped = stdout_lines[idx].strip()
        if stripped and set(stripped) == {"-"}:
            separator_indices.append(idx)
            if len(separator_indices) == 2:
                break
    if not separator_indices:
        return None, separator_indices

    name_idx = separator_indices[0] - 1
    if name_idx < 0:
        return None, separator_indices
    test_name = stdout_lines[name_idx].strip()
    return test_name or None, separator_indices


def iter_stdout_failure_blocks(stdout_lines: list[str]):
    for failure_idx, line in enumerate(stdout_lines):
        if not FAILED_HEADER_PATTERN.match(line):
            continue

        test_name, separator_indices = find_catch_failure_test_name(stdout_lines, failure_idx)
        start_idx = separator_indices[-1] if separator_indices else failure_idx

        end_idx = len(stdout_lines)
        for idx in range(failure_idx + 1, len(stdout_lines)):
            stripped = stdout_lines[idx].strip()
            if PROGRESS_TEST_START_PATTERN.match(stripped):
                end_idx = idx
                break
            if stripped.startswith("test cases:") or stripped.startswith("assertions:"):
                end_idx = idx
                break
            if stripped.startswith("==============================================================================="):
                end_idx = idx
                break
            if stripped.startswith("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"):
                end_idx = idx
                break

        block = [entry.rstrip() for entry in stdout_lines[start_idx:end_idx]]
        while block and not block[0].strip():
            block.pop(0)
        while block and not block[-1].strip():
            block.pop()
        if any(FATAL_ERROR_PATTERN.match(entry) for entry in block):
            continue
        if any(EXPLICIT_MESSAGE_PATTERN.match(entry) for entry in block):
            continue
        yield test_name, block


def extract_failed_test_names(failure: FailureInfo, stdout: str, stderr: str, batch: list[str]):
    stdout_lines = strip_skipped_test_summary_lines(strip_ansi(stdout).splitlines())
    stderr_lines = strip_skipped_test_summary_lines(strip_ansi(stderr).splitlines())
    failed_names = []

    def add_name(test_name: str | None):
        if test_name and test_name not in failed_names:
            failed_names.append(test_name)

    for line in stderr_lines:
        stripped = line.strip()
        match = FAILING_TEST_PATTERN.match(stripped)
        if match:
            add_name(match.group(1))
            continue
        match = WRONG_RESULT_PATTERN.match(stripped)
        if match and match.group(1):
            add_name(match.group(1))

    for failure_idx, line in enumerate(stdout_lines):
        if FAILED_HEADER_PATTERN.match(line):
            test_name, _ = find_catch_failure_test_name(stdout_lines, failure_idx)
            add_name(test_name)

    add_name(failure.test_name)
    if not failed_names:
        for test_name in failure.reproduce_batch or batch:
            add_name(test_name)
    return failed_names


def extract_stdout_failure_block(stdout_lines: list[str]):
    for test_name, block in iter_stdout_failure_blocks(stdout_lines):
        return test_name, block
    return None, []


def parse_stdout_assertion_failure(test_name: str | None, block: list[str]):
    for idx, line in enumerate(block):
        match = CATCH_ASSERTION_LOCATION_PATTERN.match(line.strip())
        if not match:
            continue

        source_path = match.group(1)
        source_line = int(match.group(2) or match.group(3))
        snippet_lines = render_context_snippet(source_path, source_line)
        detail_lines = []

        expression_line = None
        for next_line in block[idx + 1 :]:
            if next_line.strip():
                expression_line = next_line.strip()
                break
        if expression_line is not None:
            detail_lines.append(f"FAILED: {expression_line}")

        expansion_idx = None
        for lookahead_idx in range(idx + 1, len(block)):
            if block[lookahead_idx].strip() == "with expansion:":
                expansion_idx = lookahead_idx
                break
        if expansion_idx is not None:
            detail_lines.append("  with expansion:")
            for next_line in block[expansion_idx + 1 :]:
                if next_line.strip():
                    detail_lines.append(next_line.strip())
                    break

        score = 0
        expression_text = detail_lines[0] if detail_lines else ""
        if expression_text.startswith("FAILED: REQUIRE("):
            score += 3
        elif expression_text.startswith("FAILED:"):
            score += 2
        if "{Unknown expression after the reported line}" not in expression_text:
            score += 1
        return StdoutAssertionFailure(test_name, source_line, snippet_lines, detail_lines, score)
    return None


def parse_stdout_failure_info(stdout_lines: list[str]):
    started_tests, completed_tests = parse_stdout_progress(stdout_lines)
    last_started_test = started_tests[-1] if started_tests else None
    last_unfinished_test = None
    for test_name in started_tests:
        if test_name not in completed_tests:
            last_unfinished_test = test_name
            break

    preferred_assertion = None
    preferred_assertion_key = None
    fallback_failure_block = None
    for idx, (test_name, block) in enumerate(iter_stdout_failure_blocks(stdout_lines)):
        assertion = parse_stdout_assertion_failure(test_name, block)
        if assertion is not None:
            assertion_key = (assertion.score, idx)
            if preferred_assertion_key is None or assertion_key >= preferred_assertion_key:
                preferred_assertion_key = assertion_key
                preferred_assertion = assertion
        if fallback_failure_block is None:
            fallback_failure_block = (test_name, block)

    return StdoutParse(
        last_started_test=last_started_test,
        last_unfinished_test=last_unfinished_test,
        preferred_assertion=preferred_assertion,
        fallback_failure_block=fallback_failure_block,
        fatal_error_lines=extract_fatal_error_lines(stdout_lines),
        failed_reason_line=extract_failed_reason_line(stdout_lines),
    )


def extract_query_failure_diagnostics(stderr_lines: list[str]):
    for idx, line in enumerate(stderr_lines):
        stripped = line.strip()
        if not stripped.startswith("Query failed with message:") and not stripped.startswith("INTERNAL Error:"):
            continue

        block = [stripped]
        seen_stack_trace = False
        for next_line in stderr_lines[idx + 1 :]:
            stripped = next_line.strip()
            if not stripped:
                if seen_stack_trace:
                    continue
                continue
            if stripped.startswith("This error signals an assertion failure within DuckDB."):
                break
            if stripped.startswith("For more information, see "):
                break
            if stripped == "Stack Trace:":
                seen_stack_trace = True
                block.append(stripped)
                continue
            if seen_stack_trace:
                if re.match(r"^\d+\s+", stripped):
                    block.append(stripped)
                    continue
                break
            block.append(stripped)
        return block

    return []


def extract_failing_stderr_block(stderr_lines: list[str]):
    start_idx = None
    for idx, line in enumerate(stderr_lines):
        stripped = line.strip()
        if FAILING_TEST_PATTERN.match(stripped) or WRONG_RESULT_PATTERN.match(stripped):
            start_idx = idx
            break
    if start_idx is None:
        return []

    end_idx = len(stderr_lines)
    for idx in range(start_idx + 1, len(stderr_lines)):
        if stderr_lines[idx].startswith("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"):
            end_idx = idx
            break

    block = [line.rstrip() for line in stderr_lines[start_idx:end_idx]]
    while block and not block[0].strip():
        block.pop(0)
    while block and not block[-1].strip():
        block.pop()
    return block


def extract_wrong_result_sections(stderr_lines: list[str]):
    block = extract_failing_stderr_block(stderr_lines)
    if not block:
        return [], []

    expected_lines = []
    actual_lines = []
    current_section = None

    for line in block:
        stripped = line.strip()
        if stripped == "Expected result:":
            current_section = expected_lines
            continue
        if stripped == "Actual result:":
            current_section = actual_lines
            continue
        if current_section is None:
            continue
        if stripped and set(stripped) == {"="}:
            continue
        current_section.append(line)

    while expected_lines and not expected_lines[0].strip():
        expected_lines.pop(0)
    while expected_lines and not expected_lines[-1].strip():
        expected_lines.pop()
    while actual_lines and not actual_lines[0].strip():
        actual_lines.pop(0)
    while actual_lines and not actual_lines[-1].strip():
        actual_lines.pop()

    return expected_lines, actual_lines


def parse_stderr_failure_info(stderr_lines: list[str], batch):
    test_name, line_number = find_failing_test(stderr_lines, batch)
    stderr_non_empty_lines = [line.strip() for line in stderr_lines if line.strip()]
    wrong_result_line = next((line for line in stderr_non_empty_lines if WRONG_RESULT_PATTERN.match(line)), None)
    mismatch_context = next((line for line in stderr_non_empty_lines if line.startswith("Mismatch on row ")), None)
    mismatch_line = next((line for line in stderr_non_empty_lines if "<>" in line), None)
    expected_result_lines, actual_result_lines = extract_wrong_result_sections(stderr_lines)
    query_failure_lines = extract_query_failure_diagnostics(stderr_lines)
    first_error_line = next(
        (line.removeprefix("Error: ").strip() for line in stderr_non_empty_lines if line.startswith("Error: ")),
        None,
    )
    return StderrParse(
        test_name=test_name,
        line_number=line_number,
        failing_summary_block=extract_failing_stderr_block(stderr_lines),
        wrong_result_line=wrong_result_line,
        mismatch_context=mismatch_context,
        mismatch_line=mismatch_line,
        expected_result_lines=expected_result_lines,
        actual_result_lines=actual_result_lines,
        query_failure_lines=query_failure_lines,
        first_error_line=first_error_line,
    )


def extract_interesting_failure_block(lines: list[str]):
    for idx, line in enumerate(lines):
        if not SANITIZER_PATTERN.search(line):
            continue
        block = [next_line.strip() for next_line in lines[idx:]]
        while block and not block[-1]:
            block.pop()
        return block

    for idx, line in enumerate(lines):
        if not SANITIZER_OR_ASSERT_PATTERN.search(line):
            continue
        block = []
        for next_line in lines[idx:]:
            stripped = next_line.strip()
            if not stripped:
                if block:
                    break
                continue
            block.append(stripped)
            if len(block) >= 3:
                break
        if block:
            return block
    return []


def format_signal_summary(returncode: int | None):
    if returncode is None or returncode >= 0:
        return None
    signal_number = -returncode
    try:
        signal_name = signal.Signals(signal_number).name
    except ValueError:
        signal_name = f"SIG{signal_number}"
    description = signal.strsignal(signal_number) or "terminated by signal"
    return f"{signal_name} - {description}"


def retarget_failing_test(new_test_name: str | None, test_name: str | None, line_number: int | None):
    # a line number only applies to the test it was reported for - drop it when we blame another test
    if not new_test_name:
        return test_name, line_number
    if new_test_name != test_name:
        return new_test_name, None
    return test_name, line_number


def parse_failure_info(message: str | None, stdout: str, stderr: str, batch, returncode: int | None = None):
    stderr_lines = strip_test_event_lines(strip_skipped_test_summary_lines(strip_ansi(stderr).splitlines()))
    stdout_lines = strip_test_event_lines(strip_skipped_test_summary_lines(strip_ansi(stdout).splitlines()))
    stderr_non_empty_lines = [line.strip() for line in stderr_lines if line.strip()]
    stdout_non_empty_lines = [line.strip() for line in stdout_lines if line.strip()]
    batch_test_name = batch[0] if len(batch) == 1 else None
    stdout_info = parse_stdout_failure_info(stdout_lines)
    stderr_info = parse_stderr_failure_info(stderr_lines, batch)

    if message is not None and message.startswith("batch timed out after "):
        timeout_test_name = (
            batch_test_name or stdout_info.last_unfinished_test or infer_timed_out_test_from_stdout(stdout_lines, batch)
        )
        reproduce_batch = [timeout_test_name] if timeout_test_name else list(batch)
        return FailureInfo(
            kind="timeout",
            test_name=timeout_test_name,
            line_number=None,
            mismatch_context=None,
            expected=None,
            actual=None,
            snippet_lines=[],
            expected_result_lines=[],
            actual_result_lines=[],
            detail_lines=[
                event.get("data") or "test reported an error before timing out"
                for event in parse_test_events(stdout) + parse_test_events(stderr)
                if event.get("event") == "error" or event.get("status") == "error"
            ],
            timeout_seconds=float(re.search(r"after ([0-9]+(?:\.[0-9]+)?) seconds", message).group(1)),
            reproduce_batch=reproduce_batch,
        )

    test_name = stderr_info.test_name
    line_number = stderr_info.line_number
    if stdout_info.fatal_error_lines and stdout_info.last_started_test:
        test_name, line_number = retarget_failing_test(stdout_info.last_started_test, test_name, line_number)
    if test_name is None and returncode is not None and returncode < 0:
        test_name = stdout_info.last_unfinished_test or infer_timed_out_test_from_stdout(stdout_lines, batch)
    reproduce_batch = [test_name] if test_name else list(batch)

    if stderr_info.wrong_result_line is not None:
        if line_number is None:
            location_match = WRONG_RESULT_PATTERN.match(stderr_info.wrong_result_line)
            if location_match:
                if location_match.group(1):
                    test_name = test_name or location_match.group(1)
                if location_match.group(2):
                    line_number = int(location_match.group(2))
        actual = None
        expected = None
        if stderr_info.mismatch_line is not None:
            actual, expected = [part.strip() for part in stderr_info.mismatch_line.split("<>", 1)]
        return FailureInfo(
            kind="wrong_result",
            test_name=test_name,
            line_number=line_number,
            mismatch_context=stderr_info.mismatch_context,
            expected=expected,
            actual=actual,
            snippet_lines=render_test_snippet(test_name, line_number),
            expected_result_lines=stderr_info.expected_result_lines,
            actual_result_lines=stderr_info.actual_result_lines,
            detail_lines=[],
            timeout_seconds=None,
            reproduce_batch=reproduce_batch,
        )

    detail_lines = []
    snippet_lines = []
    if stderr_info.query_failure_lines:
        detail_lines.extend(stderr_info.query_failure_lines)
    elif message is not None and message.startswith("error: Invalid Filter:"):
        detail_lines.append(message)
    elif stdout_info.fatal_error_lines:
        detail_lines.extend(stdout_info.fatal_error_lines)

    preferred_assertion = stdout_info.preferred_assertion
    if preferred_assertion is not None:
        # the assertion line number refers to the C++ source, it is only used for the assertion snippet
        test_name, line_number = retarget_failing_test(
            preferred_assertion.test_name or stdout_info.last_started_test, test_name, line_number
        )
        reproduce_batch = [test_name] if test_name else list(batch)
        snippet_lines = preferred_assertion.snippet_lines
        if preferred_assertion.detail_lines:
            if detail_lines:
                detail_lines.append("")
            detail_lines.extend(preferred_assertion.detail_lines)
    if not snippet_lines and test_name is not None and line_number is not None:
        snippet_lines = render_test_snippet(test_name, line_number)
    if not detail_lines and stderr_info.failing_summary_block:
        detail_lines.extend(stderr_info.failing_summary_block)
    if not detail_lines:
        if stdout_info.fallback_failure_block:
            stdout_failure_test_name, stdout_failure_block = stdout_info.fallback_failure_block
            test_name, line_number = retarget_failing_test(
                stdout_failure_test_name or stdout_info.last_started_test, test_name, line_number
            )
            reproduce_batch = [test_name] if test_name else list(batch)
            detail_lines.extend(stdout_failure_block)
    if not detail_lines:
        detail_lines.extend(extract_interesting_failure_block(stderr_lines))
    if not detail_lines:
        detail_lines.extend(extract_interesting_failure_block(stdout_lines))
    if message is not None:
        if not detail_lines:
            detail_lines.append(message)
    if not detail_lines and stderr_info.first_error_line is not None:
        detail_lines.append(stderr_info.first_error_line)
    if not detail_lines:
        if stdout_info.failed_reason_line is not None:
            test_name = stdout_info.last_started_test or test_name
            reproduce_batch = [test_name] if test_name else list(batch)
            detail_lines.append(stdout_info.failed_reason_line)
    if not detail_lines:
        signal_summary = format_signal_summary(returncode)
        if signal_summary is not None:
            detail_lines.append(signal_summary)
    if not detail_lines:
        first_line = next((line for line in stderr_non_empty_lines if line), None)
        if first_line is not None:
            detail_lines.append(first_line)
    if not detail_lines:
        first_line = next((line for line in stdout_non_empty_lines if line), None)
        if first_line is not None:
            detail_lines.append(first_line)
    return FailureInfo(
        kind="generic",
        test_name=test_name,
        line_number=line_number,
        mismatch_context=None,
        expected=None,
        actual=None,
        snippet_lines=snippet_lines,
        expected_result_lines=[],
        actual_result_lines=[],
        detail_lines=detail_lines,
        timeout_seconds=None,
        reproduce_batch=reproduce_batch,
    )


def render_failure_lines(failure: FailureInfo):
    if failure.kind == "timeout":
        test_name = failure.test_name or "test batch"
        return [
            f"error: timeout ({format_duration_seconds(failure.timeout_seconds)}s) for {test_name}.",
            *failure.detail_lines,
        ]

    if failure.kind == "wrong_result":
        test_name = failure.test_name or "test batch"
        lines = [*format_fail_header(test_name), ""]
        if failure.snippet_lines:
            lines.extend(["", *failure.snippet_lines])
        if failure.mismatch_context is not None:
            lines.extend(["", f"details: {failure.mismatch_context}"])
        if failure.expected_result_lines:
            lines.extend(["", "Expected result:", *failure.expected_result_lines])
        if failure.actual_result_lines:
            lines.extend(["", "Actual result:", *failure.actual_result_lines])
        return lines

    if failure.test_name:
        lines = format_fail_header(failure.test_name)
    else:
        lines = ["error: test batch failed"]
    if failure.snippet_lines:
        lines.extend(["", *failure.snippet_lines])
    if failure.detail_lines:
        lines.extend(["", *highlight_stack_trace_lines(failure.detail_lines)])
    return lines


RAW_OUTPUT_TAIL_LINE_COUNT = 100


def is_low_information_failure(failure: FailureInfo):
    # a generic failure where we could not even identify the failing test (or any detail at all) does not help
    # debugging - in that case we dump the raw unittest output so CI logs contain everything needed to diagnose it
    if failure.kind != "generic":
        return False
    if failure.snippet_lines:
        return False
    return failure.test_name is None or not failure.detail_lines


def render_raw_output_tail(stdout: str, stderr: str):
    lines = []
    for stream_name, output in (("stdout", stdout), ("stderr", stderr)):
        stream_lines = strip_test_event_lines(
            [line.rstrip() for line in strip_ansi(normalize_output(output)).splitlines()]
        )
        while stream_lines and not stream_lines[-1].strip():
            stream_lines.pop()
        if not stream_lines:
            lines.extend(["", f"--- raw unittest {stream_name}: empty ---"])
            continue
        tail = stream_lines[-RAW_OUTPUT_TAIL_LINE_COUNT:]
        header = f"--- raw unittest {stream_name}"
        if len(tail) < len(stream_lines):
            header += f" (last {len(tail)} of {len(stream_lines)} lines)"
        lines.extend(["", header + " ---", *tail])
    return lines


def render_failure_lines_with_diagnostics(failure: FailureInfo, stdout: str, stderr: str):
    lines = render_failure_lines(failure)
    if is_low_information_failure(failure):
        lines = [*lines, *render_raw_output_tail(stdout, stderr)]
    return lines


def format_batch_failure(batch, config: TestRunnerConfig, attempt_summaries, recovered: bool, retry_count: int):
    reproduce_batch = batch
    rerun_parts = [shlex.quote(format_unittest_bin_for_display(config.unittest_bin))]
    rerun_parts.extend(shlex.split(config.test_flags))
    parts = []
    if recovered:
        first_attempt = attempt_summaries[0]
        parts.extend(first_attempt.lines)
        parts.extend(["", f"recovered: passed on retry {retry_count}/{config.retry}"])
        reproduce_batch = first_attempt.reproduce_batch
    else:
        last_attempt = attempt_summaries[-1]
        parts.extend(last_attempt.lines)
        reproduce_batch = last_attempt.reproduce_batch
    rerun_parts.append(",".join(reproduce_batch))
    rerun_cmd = shlex.join(rerun_parts)
    parts.extend(["", format_dark_gray("reproduce:"), format_dark_gray(rerun_cmd), ""])
    return "\n".join(parts)


def format_unittest_bin_for_display(unittest_bin: str):
    try:
        if os.path.isabs(unittest_bin):
            return os.path.relpath(unittest_bin, os.getcwd())
    except ValueError:
        # On Windows, relpath can fail across drives. Fall back to the original path.
        return unittest_bin
    return unittest_bin


def format_test_runner_for_display(unittest_bin: str):
    runner_name = "run.py" if os.name == "nt" else "run"
    return format_unittest_bin_for_display(os.fspath(Path(unittest_bin).with_name(runner_name)))


def escape_catch_test_name(test_name: str):
    # Catch treats commas as separators even inside quoted names.
    return test_name.replace("\\", "\\\\").replace('"', '\\"').replace(",", "\\,")


def quote_catch_test_name(test_name: str):
    return f'"{escape_catch_test_name(test_name)}"'


def format_failed_tests_reproducer(
    unittest_bin: str, test_flags: str, failed_test_configs: list[str], failed_test_names: list[str]
):
    runner = format_test_runner_for_display(unittest_bin)
    command = [runner]
    if os.name == "nt":
        command.insert(0, sys.executable)
    if test_flags:
        command.extend(["--test-flags", test_flags])
    for test_config in failed_test_configs:
        command.extend(["--test-config", test_config])
    command.append(",".join(quote_catch_test_name(test_name) for test_name in failed_test_names))
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def normalize_output(output):
    if isinstance(output, bytes):
        return output.decode("utf8", errors="backslashreplace")
    return output or ""


SKIPPED_TESTS_PATTERN = re.compile(
    r"(?:All tests passed \(|All tests were skipped \(total skipped )(\d+)(?: skipped tests,|\))"
)
SKIP_REASON_PATTERN = re.compile(r"(.+):\s+(\d+)$")
MODE_SKIP_REASON_PATTERN = re.compile(r"^mode skip(?:\s+(.*\S))?\s*$")
ANSI_ESCAPE_PATTERN = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
TEST_RUNTIME_PATTERN = re.compile(r"^\[\d+/\d+\] \(\d+%\): (.+) took ([0-9]+(?:\.[0-9]+)?)s\s*$")
TEST_EVENT_MARKER = "[TEST_EVENT] "


def strip_ansi(text: str):
    return ANSI_ESCAPE_PATTERN.sub("", text)


def strip_test_event_lines(lines: list[str]):
    return [line for line in lines if TEST_EVENT_MARKER not in line]


def parse_test_runtimes(output: str):
    runtimes = []
    for line in strip_ansi(output).splitlines():
        match = TEST_RUNTIME_PATTERN.match(line.strip())
        if match:
            runtimes.append((match.group(1), float(match.group(2))))
    return runtimes


def extract_test_runtimes(stdout: str, stderr: str):
    return parse_test_runtimes(stdout) + parse_test_runtimes(stderr)


def summarize_failure_output(message: str | None, stdout: str, stderr: str, batch, returncode: int | None = None):
    failure = parse_failure_info(message, stdout, stderr, batch, returncode)
    return render_failure_lines_with_diagnostics(failure, stdout, stderr), failure.reproduce_batch


def format_failed_test_retry_target(test_name: str | None, batch_info):
    if test_name:
        return test_name
    if batch_info["batch"]:
        return batch_info["batch"][-1]
    return f"batch {batch_info['batch_idx']}"


def parse_skipped_tests_count(output: str):
    match = SKIPPED_TESTS_PATTERN.search(strip_ansi(output))
    if not match:
        return 0
    return int(match.group(1))


def parse_skipped_test_summary(output: str):
    skipped_count = 0
    reasons = {}
    in_skip_summary = False
    for line in strip_ansi(output).splitlines():
        stripped = line.strip()
        if skipped_count == 0:
            count_match = SKIPPED_TESTS_PATTERN.search(stripped)
            if count_match:
                skipped_count = int(count_match.group(1))
        if stripped == "Skipped tests for the following reasons:":
            in_skip_summary = True
            continue
        if in_skip_summary:
            if not stripped:
                in_skip_summary = False
                continue
            reason_match = SKIP_REASON_PATTERN.match(stripped)
            if not reason_match:
                in_skip_summary = False
                continue
            reasons[reason_match.group(1)] = reasons.get(reason_match.group(1), 0) + int(reason_match.group(2))
    return skipped_count, reasons


def parse_test_events(output: str):
    events = []
    for line in strip_ansi(output).splitlines():
        marker_idx = line.find(TEST_EVENT_MARKER)
        if marker_idx < 0:
            continue
        try:
            event = json.loads(line[marker_idx + len(TEST_EVENT_MARKER) :])
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def extract_skip_events(stdout: str, stderr: str):
    skipped_test_names = set()
    skipped_reason_test_names = {}
    partial_test_names = set()
    partial_reason_test_names = {}
    saw_end_event = False
    for event in parse_test_events(stdout) + parse_test_events(stderr):
        if event.get("event") != "end":
            continue
        saw_end_event = True
        test_name = event.get("name")
        if not isinstance(test_name, str):
            continue
        status = event.get("status")
        if status == "skip-requirement":
            skipped_test_names.add(test_name)
            reason = event.get("data")
            if isinstance(reason, str):
                skipped_reason_test_names.setdefault(reason, set()).add(test_name)
            continue
        reasons = event.get("partial-skip-reasons", [])
        if status != "ok" or event.get("skip-mode", 0) <= 0 or not reasons:
            continue
        partial_test_names.add(test_name)
        for reason in reasons:
            if isinstance(reason, str):
                partial_reason_test_names.setdefault(reason, set()).add(test_name)
    return (
        saw_end_event,
        skipped_test_names,
        skipped_reason_test_names,
        partial_test_names,
        partial_reason_test_names,
    )


def strip_skipped_test_summary_lines(lines: list[str]):
    filtered_lines = []
    in_skip_summary = False
    for line in lines:
        stripped = line.strip()
        if stripped == "Skipped tests for the following reasons:":
            in_skip_summary = True
            continue
        if in_skip_summary:
            if not stripped:
                in_skip_summary = False
                continue
            if SKIP_REASON_PATTERN.match(stripped):
                continue
            in_skip_summary = False
        filtered_lines.append(line)
    return filtered_lines


def extract_skipped_test_output(stdout: str, stderr: str):
    event_summary = extract_skip_events(stdout, stderr)
    saw_end_event, skipped_names, skipped_reason_names, partial_names, partial_reason_names = event_summary
    stdout_count, stdout_reasons = parse_skipped_test_summary(stdout)
    stderr_count, stderr_reasons = parse_skipped_test_summary(stderr)
    reasons = stdout_reasons or stderr_reasons
    full_skip_reasons = {
        reason: count for reason, count in reasons.items() if not MODE_SKIP_REASON_PATTERN.match(reason)
    }
    aggregate_count = stdout_count or stderr_count
    if saw_end_event and (skipped_names or aggregate_count == 0):
        skipped_reasons = {reason: len(test_names) for reason, test_names in skipped_reason_names.items()}
        return len(skipped_names), skipped_reasons, partial_names, partial_reason_names

    # Custom test commands and older event fixtures might not emit whole-test terminal events.
    return aggregate_count, full_skip_reasons, partial_names, partial_reason_names


def coverage_profile_file_pattern(profile_dir: Path):
    return os.fspath(profile_dir / "duckdb-%p-%m.profraw")


def lcov_exclude_pattern_to_regex(pattern: str):
    pattern = pattern.strip()
    if not pattern:
        return None
    pattern = pattern.replace("\\", "/")
    regex = re.escape(pattern)
    regex = regex.replace(r"\*", ".*")
    if not pattern.startswith("*"):
        regex = r"^" + regex
    if not pattern.endswith("*"):
        regex = regex + r"$"
    return regex


def build_coverage_ignore_filename_regex(source_root: Path):
    exclude_file = source_root / ".github" / "workflows" / "lcov_exclude"
    if not exclude_file.is_file():
        return None
    patterns = []
    with exclude_file.open("r", encoding="utf8") as f:
        for line in f:
            pattern = lcov_exclude_pattern_to_regex(line)
            if pattern is not None:
                patterns.append(pattern)
    if not patterns:
        return None
    return "|".join(f"({pattern})" for pattern in patterns)


def find_executable(name: str):
    path = shutil.which(name)
    if path:
        return path
    return None


def build_dir_from_unittest_path(unittest_bin: str):
    path = Path(unittest_bin).resolve()
    parents = path.parents
    if len(parents) >= 2:
        return parents[1]
    return path.parent


def collect_coverage_objects(unittest_bin: str):
    unittest_path = Path(unittest_bin).resolve()
    build_dir = build_dir_from_unittest_path(unittest_bin)
    objects = [unittest_path]
    object_candidates = []
    object_candidates.extend((build_dir / "src").glob("libduckdb.*"))
    object_candidates.extend((build_dir / "extension").glob("**/*.duckdb_extension"))
    object_candidates.extend((build_dir / "test" / "extension").glob("**/*.duckdb_extension"))
    for candidate in sorted(object_candidates):
        if candidate.suffix not in {".dylib", ".so", ".dll", ".duckdb_extension"}:
            continue
        if candidate.is_file() and candidate not in objects:
            objects.append(candidate)
    return objects


def generate_coverage_report(source_root: Path, unittest_bin: str, profile_dir: Path, report_dir: Path):
    profraw_files = sorted(profile_dir.glob("*.profraw"))
    if not profraw_files:
        print(f"warning: no coverage profiles found in {profile_dir}")
        return 0

    llvm_profdata = find_executable("llvm-profdata")
    llvm_cov = find_executable("llvm-cov")
    genhtml = find_executable("genhtml")
    if llvm_profdata is None:
        print("error: llvm-profdata not found in PATH", file=sys.stderr)
        return 1
    if llvm_cov is None:
        print("error: llvm-cov not found in PATH", file=sys.stderr)
        return 1
    if genhtml is None:
        print("error: genhtml not found in PATH; run `brew install lcov`", file=sys.stderr)
        return 1

    lcov_config_file = source_root / ".github" / "workflows" / "lcovrc"
    if not lcov_config_file.is_file():
        print(f"error: lcov config file not found at {lcov_config_file}", file=sys.stderr)
        return 1
    coverage_css_file = source_root / "scripts" / "ci" / "codecov.css"
    if not coverage_css_file.is_file():
        print(f"error: coverage stylesheet not found at {coverage_css_file}", file=sys.stderr)
        return 1

    report_dir.mkdir(parents=True, exist_ok=True)
    merged_profile = profile_dir / "merged.profdata"
    merge_command = [llvm_profdata, "merge", "-sparse", "--failure-mode=all", "-o", os.fspath(merged_profile)]
    merge_command.extend(os.fspath(path) for path in profraw_files)
    merge_proc = subprocess.run(merge_command, check=False)
    if merge_proc.returncode != 0:
        return merge_proc.returncode

    objects = collect_coverage_objects(unittest_bin)
    lcov_info = report_dir / "lcov.info"
    export_command = [
        llvm_cov,
        "export",
        os.fspath(objects[0]),
        f"--instr-profile={merged_profile}",
        "--format=lcov",
    ]
    for obj in objects[1:]:
        export_command.append(f"--object={obj}")
    ignore_regex = build_coverage_ignore_filename_regex(source_root)
    if ignore_regex is not None:
        export_command.append(f"--ignore-filename-regex={ignore_regex}")

    with lcov_info.open("w", encoding="utf8") as lcov_file:
        export_proc = subprocess.run(export_command, stdout=lcov_file, check=False)
    if export_proc.returncode != 0:
        return export_proc.returncode

    genhtml_proc = subprocess.run(
        [
            genhtml,
            "--config-file",
            os.fspath(lcov_config_file),
            "--css-file",
            os.fspath(coverage_css_file),
            "--quiet",
            "--hierarchical",
            "--no-function-coverage",
            "--filter",
            "region,brace",
            "--ignore-errors",
            "inconsistent,inconsistent",
            "--ignore-errors",
            "corrupt,corrupt",
            "--ignore-errors",
            "unsupported,unsupported",
            "--ignore-errors",
            "mismatch,mismatch",
            "--prefix",
            os.fspath(source_root),
            "-o",
            os.fspath(report_dir),
            os.fspath(lcov_info),
        ],
        check=False,
    )
    if genhtml_proc.returncode == 0:
        print(f"coverage report: {report_dir / 'index.html'}")
    return genhtml_proc.returncode


def run_batch(config: TestRunnerConfig, batch):
    failed = False
    stdout = ""
    stderr = ""
    message = None
    allow_retry = True
    timed_out = False
    peak_rss_bytes = 0
    child_env = os.environ.copy()
    # Omit printing "FAILURES SUMMARY" block at the end of each unittest process.
    child_env["SUMMARIZE_FAILURES"] = "0"
    if config.coverage_profile_dir is not None:
        config.coverage_profile_dir.mkdir(parents=True, exist_ok=True)
        child_env["LLVM_PROFILE_FILE"] = coverage_profile_file_pattern(config.coverage_profile_dir)

    # On Windows the child process cannot reopen a NamedTemporaryFile while it
    # is still open here, so keep it after close and unlink it ourselves.
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", delete=False) as batch_file:
        # SQL-only batches are loaded as literal paths by TryReadExactSQLLogicTestFilter.
        # Quote other batches so every entry goes through Catch, including SQL paths.
        if all(name.endswith((".test", ".test_slow", ".test_coverage")) for name in batch):
            batch_file.write("\n".join(batch))
        else:
            batch_file.write("\n".join(quote_catch_test_name(name) for name in batch))
        batch_file.write("\n")
        batch_file.flush()
        batch_file_path = Path(batch_file.name)

    command = build_test_command(config, shlex.quote(str(batch_file_path)))
    # Capture the child's stdout/stderr to temp files rather than pipes, avoid deadlock
    # between a process.wait() and a full unread pipe.
    stdout_capture = tempfile.NamedTemporaryFile(mode="w+b", delete=False)
    stderr_capture = tempfile.NamedTemporaryFile(mode="w+b", delete=False)

    def read_captures():
        stdout_capture.seek(0)
        stderr_capture.seek(0)
        out = normalize_output(stdout_capture.read().decode("utf8", "backslashreplace"))
        err = normalize_output(stderr_capture.read().decode("utf8", "backslashreplace"))
        return out, err

    try:
        proc = subprocess.Popen(
            shlex.split(command),
            stdout=stdout_capture,
            stderr=stderr_capture,
            env=child_env,
        )
        deadline = time.monotonic() + config.batch_timeout_seconds

        while proc.poll() is None:
            if stop_requested():
                proc.kill()
                proc.wait()
                stdout, stderr = read_captures()
                failed = True
                allow_retry = False
                message = "interrupted"
                break
            rss_bytes = get_process_rss_bytes(proc.pid)
            if rss_bytes is not None:
                peak_rss_bytes = max(peak_rss_bytes, rss_bytes)
            if time.monotonic() >= deadline:
                proc.kill()
                proc.wait()
                stdout, stderr = read_captures()
                failed = True
                timed_out = proc.returncode == (1 if os.name == "nt" else -signal.SIGKILL)
                message = f"batch timed out after {config.batch_timeout_seconds} seconds"
                break
            if proc.poll() is None:
                time.sleep(DEFAULT_RSS_POLL_INTERVAL_SECONDS)

        if message is None:
            proc.wait()
            stdout, stderr = read_captures()
            rss_bytes = get_process_rss_bytes(proc.pid)
            if rss_bytes is not None:
                peak_rss_bytes = max(peak_rss_bytes, rss_bytes)
            failed = proc.returncode != 0
            invalid_filters = [line for line in strip_ansi(stdout).splitlines() if line.startswith("Invalid Filter:")]
            if not failed and invalid_filters:
                failed = True
                allow_retry = False
                message = "error: " + invalid_filters[0]
            if not failed and config.fail_require_skip:
                require_skip_lines = find_require_skip_lines(stdout) + find_require_skip_lines(stderr)
                if require_skip_lines:
                    failed = True
                    message = "error: detected require-based skipped tests: " + require_skip_lines[0]

                    allow_retry = False
    except OSError as exc:
        failed = True
        stderr = str(exc)
        message = "failed to launch batch command"
    finally:
        batch_file_path.unlink(missing_ok=True)
        stdout_capture.close()
        stderr_capture.close()
        Path(stdout_capture.name).unlink(missing_ok=True)
        Path(stderr_capture.name).unlink(missing_ok=True)

    return {
        "failed": failed,
        "stdout": stdout,
        "stderr": stderr,
        "message": message,
        "returncode": proc.returncode if "proc" in locals() else None,
        "peak_rss_bytes": peak_rss_bytes,
        "allow_retry": allow_retry,
        "timed_out": timed_out,
    }


def find_require_skip_lines(output: str):
    ansi = r"(?:\x1b\[[0-9;]*m)*"
    pattern = rf"^{ansi}(require\s+\S+:\s+\d+){ansi}\s*$"
    return re.findall(pattern, output, flags=re.MULTILINE)


def submit_batch(executor, config: TestRunnerConfig, batch, future_to_batch, batch_idx: int, attempt: int):
    future = executor.submit(run_batch, config, batch)
    future_to_batch[future] = {
        "batch_idx": batch_idx,
        "batch": batch,
        "start": time.monotonic(),
        "attempt": attempt,
    }


def submit_batches(executor, config: TestRunnerConfig, batches, future_to_batch, next_batch_idx: int):
    while next_batch_idx < len(batches) and len(future_to_batch) < config.workers:
        batch = batches[next_batch_idx]
        submit_batch(executor, config, batch, future_to_batch, next_batch_idx, 0)
        next_batch_idx += 1
    return next_batch_idx


def can_warn_timeout(config: TestRunnerConfig, batch, result):
    if config.timeout_policy != "warn" or len(batch) != 1 or not result.get("timed_out") or stop_requested():
        return False
    stdout, stderr = result["stdout"], result["stderr"]
    events = parse_test_events(stdout) + parse_test_events(stderr)
    # Old binaries do not report the body/cleanup boundary. Missing or incomplete events fail closed.
    if len(events) != 2 or stdout.count(TEST_EVENT_MARKER) + stderr.count(TEST_EVENT_MARKER) != 2:
        return False
    begin = events[0]
    if begin.get("event") != "begin" or begin.get("version") != 2 or begin.get("name") != batch[0]:
        return False
    if events[1].get("event") != "execution-begin" or events[1].get("name") != batch[0]:
        return False
    stdout_lines = strip_test_event_lines(strip_ansi(stdout).splitlines())
    stderr_lines = strip_test_event_lines(strip_ansi(stderr).splitlines())
    out = parse_stdout_failure_info(stdout_lines)
    err = parse_stderr_failure_info(stderr_lines, batch)
    return not any(
        (
            out.preferred_assertion,
            out.fallback_failure_block,
            out.fatal_error_lines,
            out.failed_reason_line,
            err.wrong_result_line,
            err.query_failure_lines,
            err.failing_summary_block,
            err.first_error_line,
            extract_interesting_failure_block(stdout_lines + stderr_lines),
        )
    )


def handle_failed_batch(ctx: RunContext, batch_info, result):
    failure = parse_failure_info(
        result["message"],
        result["stdout"],
        result["stderr"],
        batch_info["batch"],
        result.get("returncode"),
    )
    lines = render_failure_lines_with_diagnostics(failure, result["stdout"], result["stderr"])
    reproduce_batch = failure.reproduce_batch
    failed_test_names = extract_failed_test_names(failure, result["stdout"], result["stderr"], batch_info["batch"])
    if batch_info["batch_idx"] not in ctx.state.failed_attempts and can_warn_timeout(
        ctx.config, batch_info["batch"], result
    ):
        test_name = batch_info["batch"][0]
        ctx.state.timed_out_test_names.append(test_name)
        ctx.progress.print_message(
            f"warn: timeout ({format_duration_seconds(failure.timeout_seconds)}s) for {test_name}; not retried."
        )
        ctx.progress.print_message(
            "reproduce: "
            + shlex.join(
                [
                    format_unittest_bin_for_display(ctx.config.unittest_bin),
                    *shlex.split(ctx.config.test_flags),
                    test_name,
                ]
            )
        )
        return False
    ctx.state.add_failed_attempt(batch_info["batch_idx"], lines, reproduce_batch, failed_test_names)
    retry_target = format_failed_test_retry_target(failure.test_name, batch_info)
    if result.get("allow_retry", True) and ctx.state.can_retry(batch_info, ctx.config):
        ctx.state.record_retry()
        next_attempt = batch_info["attempt"] + 1
        ctx.progress.print_message(
            f"retrying failed test {retry_target} "
            f"(attempt {next_attempt}/{ctx.config.retry}, retry {ctx.state.retry_count}/{ctx.config.max_retries})"
        )
        submit_batch(
            ctx.executor,
            ctx.config,
            batch_info["batch"],
            ctx.future_to_batch,
            batch_info["batch_idx"],
            next_attempt,
        )
        return True

    if result.get("allow_retry", True) and batch_info["attempt"] < ctx.config.retry:
        ctx.progress.print_message(
            f"not retrying failed test {retry_target} after reaching {ctx.config.max_retries} retries"
        )

    attempt_summaries = ctx.state.pop_failed_attempts(batch_info["batch_idx"])
    ctx.state.record_failed_test_names(attempt_summaries)
    ctx.progress.print_message(
        format_batch_failure(
            batch_info["batch"],
            ctx.config,
            attempt_summaries,
            recovered=False,
            retry_count=batch_info["attempt"],
        )
    )
    ctx.state.record_failure()
    if ctx.state.should_stop(ctx.config):
        ctx.state.stop_launching = True
    return False


def report_batch_metrics(ctx: RunContext, batch_info, result, elapsed: float):
    if ctx.config.runtime_threshold_seconds is not None:
        test_runtimes = extract_test_runtimes(result["stdout"], result["stderr"])
        if test_runtimes:
            for test_name, test_elapsed in test_runtimes:
                if test_elapsed >= ctx.config.runtime_threshold_seconds:
                    ctx.progress.print_message(f"warn: {test_name} took {test_elapsed:.2f}s")
        elif elapsed >= ctx.config.runtime_threshold_seconds:
            ctx.progress.print_message(f"warn: {batch_info['batch'][0]} took {elapsed:.2f}s")
    if (
        ctx.config.rss_memory_threshold_mib is not None
        and format_mib(result["peak_rss_bytes"]) >= ctx.config.rss_memory_threshold_mib
    ):
        ctx.progress.print_message(
            f"batch with file {batch_info['batch'][0]} peak RSS {format_mib(result['peak_rss_bytes']):.0f} MiB"
        )


def parse_args(argv: list[str] | None = None):
    if argv is None:
        argv = sys.argv[1:]
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-list", type=Path)
    parser.add_argument("--changed-tests", type=Path, help="extra test list file; requires --test-list")
    parser.add_argument(
        "--stabilize-tests",
        action="store_true",
        help="rerun selected tests with stabilization logic (fast/slow repetition policy)",
    )
    parser.add_argument("--workers", default=DEFAULT_WORKERS)
    parser.add_argument(
        "--test-config",
        action="append",
        default=[],
        help="path to test config; may be passed multiple times and runs each config independently",
    )
    parser.add_argument(
        "--test-flags",
        default="",
        help="additional flags appended to the unittest binary for listing and execution",
    )
    parser.add_argument("unittest_bin", nargs="?", help=argparse.SUPPRESS)
    parser.add_argument("patterns", nargs="*")
    parser.add_argument(
        "--test-command",
        default="{binary} {flags} --use-colour yes -f {test_list}",
        help="shell command template used to run a test batch; supports {binary}, {flags}, and {test_list}",
    )
    parser.add_argument(
        "--track-runtime",
        type=float,
        nargs="?",
        const=DEFAULT_RUNTIME_THRESHOLD_SECONDS,
        default=None,
    )
    parser.add_argument(
        "--track-rss-memory",
        type=int,
        nargs="?",
        const=DEFAULT_RSS_MEMORY_THRESHOLD_MIB,
        default=None,
        help="print batches whose peak RSS meets or exceeds the threshold in MiB (default: 1024)",
    )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--max-failures", type=int)
    parser.add_argument("--retry", type=int, default=0)
    parser.add_argument("--max-retries", type=int, default=DEFAULT_MAX_RETRIES)
    parser.add_argument("--batch-size", type=int, default=DEFAULT_BATCH_SIZE)
    parser.add_argument("--batch-timeout", type=float)
    parser.add_argument(
        "--timeout-policy",
        choices=("fail", "warn"),
        default="fail",
        help="warn about test-body timeouts without retrying or failing; requires --batch-size 1",
    )
    parser.add_argument(
        "--fail-require-skip",
        action="store_true",
        help="fail a batch if unittest output reports skipped tests for `require ...` reasons",
    )
    parser.add_argument("--coverage-report", type=Path, help="write an LCOV HTML coverage report to this directory")
    # Accept options interleaved with positional patterns, e.g.:
    #   run_tests.py bin "[tag]" --fail-fast test/sql/foo.test
    return parser.parse_intermixed_args(argv)


@dataclass(frozen=True)
class InvocationResult:
    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class ConfigInvocation:
    label: str
    test_flags: str
    test_config: str | None


@dataclass(frozen=True)
class ConfigRunResult:
    returncode: int
    passed_tests: int
    failed_tests: int
    skipped_tests: int
    elapsed_seconds: float
    partially_skipped_tests: int = 0
    failed_test_names: tuple[str, ...] = ()
    timed_out_test_names: tuple[str, ...] = ()
    not_run_tests: int = 0


def build_test_flags(base_flags: str, test_config: str | None):
    if not test_config:
        return base_flags
    config_flag = f"--test-config {shlex.quote(test_config)}"
    return " ".join(flag for flag in [base_flags, config_flag] if flag)


def build_config_invocations(test_configs: list[str], base_flags: str):
    if not test_configs:
        return [ConfigInvocation(label="default", test_flags=base_flags, test_config=None)]
    return [
        ConfigInvocation(
            label=test_config, test_flags=build_test_flags(base_flags, test_config), test_config=test_config
        )
        for test_config in test_configs
    ]


def create_temp_test_list(
    unittest_bin: str,
    test_flags: str,
    patterns: list[str],
    test_list_files: list[Path] | None,
    coverage_profile_dir: Path | None = None,
    error_output=None,
):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", delete=False) as test_file:
        generate_test_list(
            test_file,
            unittest_bin,
            test_flags,
            patterns,
            test_list_files,
            coverage_profile_dir,
            error_output,
        )
        return Path(test_file.name)


def run_single_config(
    args,
    unittest_bin: str,
    workers: int,
    retry: int,
    max_retries: int,
    max_failures: int | None,
    batch_size: int,
    test_list_files: list[Path],
    invocation: ConfigInvocation,
    print_config_header: bool,
    executor: concurrent.futures.ThreadPoolExecutor | None = None,
    output=None,
    tail_callback=None,
):
    output = output if output is not None else sys.stdout
    if stop_requested():
        return ConfigRunResult(returncode=130, passed_tests=0, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0)
    if print_config_header:
        print(f"=== config run: {invocation.label} ===", file=output)
    generated_test_list: Path | None = None
    try:
        if args.test_list is not None and len(test_list_files) == 1:
            test_list_path = args.test_list
        else:
            create_args = (
                unittest_bin,
                invocation.test_flags,
                args.patterns,
                test_list_files,
                args.coverage_profile_dir,
                output,
            )
            if executor is None:
                generated_test_list = create_temp_test_list(*create_args)
            else:
                generated_test_list = executor.submit(create_temp_test_list, *create_args).result()
            test_list_path = generated_test_list
        config = TestRunnerConfig(
            test_list=test_list_path,
            unittest_bin=unittest_bin,
            test_flags=invocation.test_flags,
            patterns=args.patterns,
            test_command=args.test_command,
            workers=workers,
            retry=retry,
            max_retries=max_retries,
            batch_size=batch_size,
            batch_timeout_seconds=args.batch_timeout,
            rss_memory_threshold_mib=args.track_rss_memory,
            runtime_threshold_seconds=args.track_runtime,
            max_failures=max_failures,
            fail_require_skip=args.fail_require_skip,
            coverage_profile_dir=args.coverage_profile_dir,
            timeout_policy=args.timeout_policy,
        )

        tests = load_tests(config.test_list)
        if stop_requested():
            return ConfigRunResult(returncode=130, passed_tests=0, failed_tests=0, skipped_tests=0, elapsed_seconds=0.0)
        if len(tests) == 0:
            print(f"error: no tests selected for config '{invocation.label}'", file=output)
            return ConfigRunResult(returncode=1, passed_tests=0, failed_tests=1, skipped_tests=0, elapsed_seconds=0.0)
        stabilization_tests = []
        if args.changed_tests is not None:
            merged_names = {test.name for test in tests}
            base_names = {test.name for test in load_tests(args.test_list)}
            changed_test_names = merged_names - base_names
            added_test_count = len(changed_test_names)
            print(f"added {added_test_count} tests from --changed-tests file to the smoke test run", file=output)
            changed_test_name_set = set(changed_test_names)
            stabilization_tests = [test for test in tests if test.name in changed_test_name_set]
        elif args.stabilize_tests:
            stabilization_tests = tests
        computed_batch_size = compute_batch_size(len(tests), config)

        config_values = asdict(config)
        config_values["batch_size"] = computed_batch_size
        config_values.pop("test_list", None)
        config_values.pop("unittest_bin", None)
        config_values.pop("test_command", None)
        config_values.pop("coverage_profile_dir", None)
        config_values = {k: v for k, v in config_values.items() if v is not None and v != "" and v != []}
        config_output = ", ".join(f"{key}={value}" for key, value in config_values.items())
        print(f"config: {config_output}", file=output)

        def run_test_batches(test_batches, test_count, notify_tail=False):
            if executor is None and output is sys.stdout:
                return run_tests(config, test_batches, test_count)
            return run_tests(
                config,
                test_batches,
                test_count,
                executor=executor,
                output=output,
                tail_callback=tail_callback if notify_tail else None,
            )

        batches = list(chunked(tests, computed_batch_size))
        initial_run_result = run_test_batches(batches, len(tests), notify_tail=True)
        if initial_run_result.returncode != 0 or not stabilization_tests:
            return initial_run_result

        fast_tests, slow_tests = split_fast_slow_tests(stabilization_tests)
        fast_extra_runs, slow_extra_runs = stabilization_extra_runs(len(stabilization_tests))
        print(
            "stabilizing tests: "
            f"{len(stabilization_tests)} changed/selected tests "
            f"({len(fast_tests)} fast, {len(slow_tests)} slow), "
            f"extra reruns fast={fast_extra_runs}, slow={slow_extra_runs}",
            file=output,
        )

        stabilization_failed = False
        stabilization_failed_test_names = []
        for rerun_idx in range(max(fast_extra_runs, slow_extra_runs)):
            rerun_round = rerun_idx + 1
            if rerun_idx < fast_extra_runs and fast_tests:
                print(f"stabilization rerun {rerun_round}/{fast_extra_runs} for fast tests", file=output)
                fast_batches = list(chunked(fast_tests, compute_batch_size(len(fast_tests), config)))
                fast_result = run_test_batches(fast_batches, len(fast_tests))
                if fast_result.returncode != 0:
                    stabilization_failed = True
                    stabilization_failed_test_names.extend(fast_result.failed_test_names)
            if rerun_idx < slow_extra_runs and slow_tests:
                print(f"stabilization rerun {rerun_round}/{slow_extra_runs} for slow tests", file=output)
                slow_batches = list(chunked(slow_tests, compute_batch_size(len(slow_tests), config)))
                slow_result = run_test_batches(slow_batches, len(slow_tests))
                if slow_result.returncode != 0:
                    stabilization_failed = True
                    stabilization_failed_test_names.extend(slow_result.failed_test_names)
            if stabilization_failed:
                break

        if stabilization_failed:
            print("error: stabilization rerun failure detected", file=output)
            return ConfigRunResult(
                returncode=1,
                passed_tests=initial_run_result.passed_tests,
                failed_tests=max(1, initial_run_result.failed_tests),
                skipped_tests=initial_run_result.skipped_tests,
                elapsed_seconds=initial_run_result.elapsed_seconds,
                partially_skipped_tests=initial_run_result.partially_skipped_tests,
                failed_test_names=tuple(dict.fromkeys(stabilization_failed_test_names)),
            )

        return initial_run_result
    finally:
        if generated_test_list is not None:
            generated_test_list.unlink(missing_ok=True)


def failed_config_run_result():
    return ConfigRunResult(returncode=1, passed_tests=0, failed_tests=1, skipped_tests=0, elapsed_seconds=0.0)


def print_config_run_summary(run_result: ConfigRunResult):
    if run_result.returncode not in (0, 1):
        return
    partial_skip_suffix = ""
    if run_result.partially_skipped_tests:
        partial_skip_suffix = f" ({run_result.partially_skipped_tests} partially)"
    if run_result.timed_out_test_names:
        partial_skip_suffix += f", {len(run_result.timed_out_test_names)} timed out"
    if run_result.not_run_tests:
        partial_skip_suffix += f", {run_result.not_run_tests} not run"
    if run_result.failed_tests > 0:
        print(
            "❌ ran tests: "
            f"{run_result.passed_tests} passed, {run_result.failed_tests} failed, "
            f"{run_result.skipped_tests} skipped{partial_skip_suffix} in {run_result.elapsed_seconds:.0f}s"
        )
    else:
        print(
            "ran tests: "
            f"{run_result.passed_tests} passed, {run_result.skipped_tests} skipped{partial_skip_suffix} "
            f"in {run_result.elapsed_seconds:.0f}s"
        )


def print_buffered_config_run(
    invocation: ConfigInvocation, output: StringIO, run_result: ConfigRunResult, use_config_groups: bool
):
    if use_config_groups:
        print(f"::group::test config: {invocation.label}")
    else:
        print(f"=== config run: {invocation.label} ===")
    contents = output.getvalue()
    if contents:
        print(contents, end="" if contents.endswith("\n") else "\n")
    if use_config_groups:
        print("::endgroup::")
    print_config_run_summary(run_result)


def main(argv: list[str] | None = None):
    enable_line_buffering()
    STOP_REQUESTED.clear()
    previous_sigint_handler = signal.getsignal(signal.SIGINT)
    signal.signal(signal.SIGINT, signal_stop_requested)
    try:
        return main_impl(argv)
    finally:
        signal.signal(signal.SIGINT, previous_sigint_handler)


def main_impl(argv: list[str] | None = None):
    args = parse_args(argv)
    args.coverage_profile_dir = None
    if args.timeout_policy == "warn" and args.batch_size != 1:
        print("error: --timeout-policy warn requires --batch-size 1")
        return 1
    if args.timeout_policy == "warn" and (args.stabilize_tests or args.changed_tests is not None):
        print("error: --timeout-policy warn cannot be combined with test stabilization")
        return 1
    if args.changed_tests is not None and args.test_list is None:
        print("error: --changed-tests requires --test-list", file=sys.stderr)
        return 1
    if not args.unittest_bin:
        print("error: missing unittest binary", file=sys.stderr)
        return 1

    test_list_files = [path for path in [args.test_list, args.changed_tests] if path is not None]
    config_invocations = build_config_invocations(args.test_config, args.test_flags)
    is_ci = bool(os.environ.get("CI"))
    use_config_groups = is_ci and len(config_invocations) > 1
    max_failures = args.max_failures
    if args.fail_fast:
        max_failures = 1
    retry = max(0, args.retry)
    if retry == 0 and os.environ.get("CI"):
        retry = 2
        print("CI detected, enabling retry=2 per batch")
    max_retries = max(0, args.max_retries)
    workers = resolve_workers(args.workers)
    args.batch_timeout = resolve_batch_timeout(args.batch_timeout, workers)
    unittest_bin = args.unittest_bin
    if os.name == "nt":
        unittest_bin = unittest_bin.replace("/", "\\")
    batch_size = args.batch_size
    failed_configs = []
    failed_test_configs = []
    failed_test_names = []
    failed_test_name_set = set()
    if len(config_invocations) > 1:
        print(f"running {len(config_invocations)} configs with {workers} shared workers")
    coverage_profile_tmp = None
    try:
        if args.coverage_report is not None:
            coverage_profile_tmp = tempfile.TemporaryDirectory(prefix="duckdb-coverage-")
            args.coverage_profile_dir = Path(coverage_profile_tmp.name)
        config_results: list[ConfigRunResult | None] = [None] * len(config_invocations)
        if len(config_invocations) == 1:
            invocation = config_invocations[0]
            try:
                config_results[0] = run_single_config(
                    args,
                    unittest_bin,
                    workers,
                    retry,
                    max_retries,
                    max_failures,
                    batch_size,
                    test_list_files,
                    invocation,
                    invocation.label != "default",
                )
            except Exception as exc:
                print(f"error: {exc}")
                traceback.print_exc()
                config_results[0] = failed_config_run_result()
            print_config_run_summary(config_results[0])
        else:
            config_worker_count = min(len(config_invocations), workers)
            buffers = [StringIO() for _ in config_invocations]
            config_start_events = [threading.Event() for _ in config_invocations]
            config_start_events[0].set()

            def run_config_after_previous_tail(config_idx, batch_executor):
                config_start_events[config_idx].wait()
                next_config_event = (
                    config_start_events[config_idx + 1] if config_idx + 1 < len(config_invocations) else None
                )
                try:
                    return run_single_config(
                        args,
                        unittest_bin,
                        workers,
                        retry,
                        max_retries,
                        max_failures,
                        batch_size,
                        test_list_files,
                        config_invocations[config_idx],
                        False,
                        batch_executor,
                        buffers[config_idx],
                        next_config_event.set if next_config_event is not None else None,
                    )
                finally:
                    if next_config_event is not None:
                        next_config_event.set()

            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as batch_executor:
                with concurrent.futures.ThreadPoolExecutor(max_workers=config_worker_count) as config_executor:
                    future_to_config = {}
                    for config_idx in range(len(config_invocations)):
                        future = config_executor.submit(
                            run_config_after_previous_tail,
                            config_idx,
                            batch_executor,
                        )
                        future_to_config[future] = config_idx
                    for future in concurrent.futures.as_completed(future_to_config):
                        config_idx = future_to_config[future]
                        invocation = config_invocations[config_idx]
                        try:
                            run_result = future.result()
                        except Exception as exc:
                            print(f"error: {exc}", file=buffers[config_idx])
                            traceback.print_exc(file=buffers[config_idx])
                            run_result = failed_config_run_result()
                        config_results[config_idx] = run_result
                        print_buffered_config_run(invocation, buffers[config_idx], run_result, use_config_groups)

        if any(run_result is not None and run_result.returncode == 130 for run_result in config_results):
            print("interrupted")
            return 130

        for invocation, run_result in zip(config_invocations, config_results):
            if run_result is None:
                continue
            if run_result.timed_out_test_names:
                print_timeout_summary(invocation, run_result)
            if run_result.returncode != 0:
                failed_configs.append(invocation.label)
                if run_result.failed_test_names:
                    if invocation.test_config is not None:
                        failed_test_configs.append(invocation.test_config)
                    for test_name in run_result.failed_test_names:
                        if test_name not in failed_test_name_set:
                            failed_test_name_set.add(test_name)
                            failed_test_names.append(test_name)

        report_returncode = 0
        if args.coverage_report is not None:
            report_returncode = generate_coverage_report(
                Path.cwd().resolve(), unittest_bin, args.coverage_profile_dir, args.coverage_report
            )
    finally:
        if coverage_profile_tmp is not None:
            coverage_profile_tmp.cleanup()

    if failed_configs:
        if len(config_invocations) > 1:
            print(f"error: {len(failed_configs)} config runs failed: {', '.join(failed_configs)}")
        if failed_test_names:
            print()
            print("reproduce all failed tests:")
            print(format_failed_tests_reproducer(unittest_bin, args.test_flags, failed_test_configs, failed_test_names))
        return 1
    if args.coverage_report is not None and report_returncode != 0:
        return report_returncode
    if len(config_invocations) > 1:
        print(f"all {len(config_invocations)} config runs passed")
    return 0


def print_timeout_summary(invocation: ConfigInvocation, result: ConfigRunResult):
    lines = [
        f"### Timed-out tests: {invocation.label}",
        "",
        f"{len(result.timed_out_test_names)} timed out; {result.passed_tests} passed, "
        f"{result.failed_tests} failed, {result.skipped_tests} skipped, {result.not_run_tests} not run.",
        "",
        f"Revision: {os.environ.get('GITHUB_SHA', 'local')}",
        "",
        *[f"- `{name}`" for name in result.timed_out_test_names],
        "",
    ]
    print("\n".join(lines))
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf8") as summary:
            summary.write("\n".join(lines) + "\n")


def invoke(argv: list[str], cwd: Path | None = None) -> InvocationResult:
    stdout_buffer = StringIO()
    stderr_buffer = StringIO()
    old_cwd = os.getcwd()
    try:
        if cwd is not None:
            os.chdir(cwd)
        with redirect_stdout(stdout_buffer), redirect_stderr(stderr_buffer):
            returncode = int(main(argv) or 0)
    finally:
        os.chdir(old_cwd)
    return InvocationResult(returncode=returncode, stdout=stdout_buffer.getvalue(), stderr=stderr_buffer.getvalue())


def run_tests(
    config: TestRunnerConfig,
    batches,
    total_tests: int,
    executor: concurrent.futures.ThreadPoolExecutor | None = None,
    output=None,
    tail_callback=None,
):
    output = output if output is not None else sys.stdout
    start = time.monotonic()
    state = BatchRunState()
    progress = DotProgressBar(len(batches), output)
    total_skipped_tests = 0
    completed_tests = 0
    partially_skipped_test_names = set()
    skipped_reason_counts = {}
    partial_skip_reason_test_names = {}
    tail_notified = False

    def notify_tail(next_batch_idx):
        nonlocal tail_notified
        if tail_callback is not None and not tail_notified and next_batch_idx >= len(batches):
            tail_notified = True
            tail_callback()

    executor_context = (
        concurrent.futures.ThreadPoolExecutor(max_workers=config.workers)
        if executor is None
        else contextlib.nullcontext(executor)
    )
    with executor_context as active_executor:
        future_to_batch = {}
        next_batch_idx = 0
        ctx = RunContext(
            executor=active_executor,
            config=config,
            state=state,
            future_to_batch=future_to_batch,
            progress=progress,
        )

        if stop_requested():
            return ConfigRunResult(
                returncode=130,
                passed_tests=0,
                failed_tests=0,
                skipped_tests=0,
                elapsed_seconds=time.monotonic() - start,
            )
        next_batch_idx = submit_batches(active_executor, config, batches, future_to_batch, next_batch_idx)
        notify_tail(next_batch_idx)

        while future_to_batch:
            if stop_requested():
                state.stop_launching = True
                for future in future_to_batch:
                    future.cancel()
                break
            done, _ = concurrent.futures.wait(
                future_to_batch,
                timeout=0.2,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            if not done and stop_requested():
                state.stop_launching = True
                for future in future_to_batch:
                    future.cancel()
                break
            for future in done:
                batch_info = future_to_batch.pop(future)
                result = future.result()
                elapsed = time.monotonic() - batch_info["start"]
                report_batch_metrics(ctx, batch_info, result, elapsed)
                if result["failed"]:
                    if handle_failed_batch(ctx, batch_info, result):
                        continue
                    skipped_count, skipped_reasons, partial_names, partial_reason_names = extract_skipped_test_output(
                        result["stdout"], result["stderr"]
                    )
                    total_skipped_tests += skipped_count
                    partially_skipped_test_names.update(partial_names)
                    for reason, count in skipped_reasons.items():
                        skipped_reason_counts[reason] = skipped_reason_counts.get(reason, 0) + count
                    for reason, test_names in partial_reason_names.items():
                        partial_skip_reason_test_names.setdefault(reason, set()).update(test_names)
                else:
                    attempt_summaries = ctx.state.pop_failed_attempts(batch_info["batch_idx"])
                    if attempt_summaries:
                        ctx.progress.print_message(
                            format_batch_failure(
                                batch_info["batch"],
                                ctx.config,
                                attempt_summaries,
                                recovered=True,
                                retry_count=batch_info["attempt"],
                            )
                        )
                    skipped_count, skipped_reasons, partial_names, partial_reason_names = extract_skipped_test_output(
                        result["stdout"], result["stderr"]
                    )
                    total_skipped_tests += skipped_count
                    partially_skipped_test_names.update(partial_names)
                    for reason, count in skipped_reasons.items():
                        skipped_reason_counts[reason] = skipped_reason_counts.get(reason, 0) + count
                    for reason, test_names in partial_reason_names.items():
                        partial_skip_reason_test_names.setdefault(reason, set()).update(test_names)
                completed_tests += len(batch_info["batch"])
                progress.advance(next_batch_idx - len(future_to_batch))

            if not state.stop_launching and not stop_requested():
                next_batch_idx = submit_batches(active_executor, config, batches, future_to_batch, next_batch_idx)
                notify_tail(next_batch_idx)

    progress.flush_line()
    elapsed = time.monotonic() - start
    total_partially_skipped_tests = len(partially_skipped_test_names)
    if stop_requested():
        return ConfigRunResult(returncode=130, passed_tests=0, failed_tests=0, skipped_tests=0, elapsed_seconds=elapsed)
    exit_code = 0
    if state.failed_count:
        exit_code = 1
    elif state.timed_out_test_names:
        print(f"finished in {elapsed:.0f}s with {len(state.timed_out_test_names)} timeout warnings", file=output)
    elif total_skipped_tests or total_partially_skipped_tests:
        skip_parts = []
        if total_skipped_tests:
            skip_parts.append(f"{total_skipped_tests} skipped tests")
        if total_partially_skipped_tests:
            noun = "test" if total_partially_skipped_tests == 1 else "tests"
            skip_parts.append(f"{total_partially_skipped_tests} partially skipped {noun}")
        print(f"all tests passed in {elapsed:.0f}s ({', '.join(skip_parts)})", file=output)
    else:
        print(f"all tests passed in {elapsed:.0f}s", file=output)
    if skipped_reason_counts:
        print(file=output)
        print(f"Skipped tests ({total_skipped_tests}):", file=output)
        for reason in sorted(skipped_reason_counts):
            print(f"{reason}: {skipped_reason_counts[reason]}", file=output)
    if total_partially_skipped_tests:
        print(file=output)
        print(f"Partially skipped tests ({total_partially_skipped_tests}):", file=output)
        for reason in sorted(partial_skip_reason_test_names):
            print(f"{reason}: {len(partial_skip_reason_test_names[reason])}", file=output)
    failed_tests = state.failed_count
    passed_tests = max(0, completed_tests - failed_tests - total_skipped_tests - len(state.timed_out_test_names))
    failed_test_names = []
    for batch in batches:
        for test_name in batch:
            if test_name in state.failed_test_names and test_name not in failed_test_names:
                failed_test_names.append(test_name)
    failed_test_names.extend(sorted(state.failed_test_names.difference(failed_test_names)))
    return ConfigRunResult(
        returncode=exit_code,
        passed_tests=passed_tests,
        failed_tests=failed_tests,
        skipped_tests=total_skipped_tests,
        elapsed_seconds=elapsed,
        partially_skipped_tests=total_partially_skipped_tests,
        failed_test_names=tuple(failed_test_names),
        timed_out_test_names=tuple(state.timed_out_test_names),
        not_run_tests=max(0, total_tests - completed_tests),
    )


if __name__ == "__main__":
    raise SystemExit(main())
