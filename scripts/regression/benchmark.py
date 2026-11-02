import csv
import os
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple


BENCHMARK_DATA_DIRECTORY = "duckdb_benchmark_data"
DEFAULT_PROCESS_TIMEOUT = 600
DISABLED_RUNNER_TIMEOUT = 3600
EXTENSION_DIRECTORY_ENV = "DUCKDB_BENCHMARK_EXTENSION_DIRECTORY"
BENCHMARK_NOT_FOUND_MESSAGE = "Benchmark to run could not be found."

STDERR_HEADER = '''====================================================
==============         STDERR          =============
====================================================
'''

STDOUT_HEADER = '''====================================================
==============         STDOUT          =============
====================================================
'''


def benchmark_not_found(error: Optional[str]) -> bool:
    return error is not None and BENCHMARK_NOT_FOUND_MESSAGE in (line.strip() for line in error.splitlines())


def benchmark_failure_message(
    label: str, benchmark: str, row: List[str], trailing_lines: List[str], stdout: str
) -> str:
    run = row[1].strip() if len(row) > 1 else ""
    status = row[2].strip() if len(row) > 2 else "malformed output"
    location = f" for {benchmark}"
    if run:
        location += f" on run {run}"

    details = "\n".join(trailing_lines).strip()
    if not details:
        details = stdout.strip()
    message = f"{label} benchmark runner reported {status}{location}"
    if details:
        message += f":\n{details}"
    return message


def find_extension_directory(runner_path: str) -> Optional[str]:
    release_directory = Path(runner_path).resolve().parent.parent
    repository_directory = release_directory / "repository"
    extension_directories = sorted(
        {extension_path.parent for extension_path in repository_directory.glob("*/*/*.duckdb_extension")}
    )
    if not extension_directories:
        return None
    if len(extension_directories) != 1:
        directories = ", ".join(str(path) for path in extension_directories)
        raise ValueError(f"Found multiple extension directories for {runner_path}: {directories}")
    return str(extension_directories[0])


def find_benchmark_cache_directory(runner_path: str) -> str:
    return os.path.abspath(os.path.join(os.path.dirname(runner_path), "..", "..", "..", BENCHMARK_DATA_DIRECTORY))


def symlink_directory_entries(source_directory: Path, target_directory: Path, skipped_name: Optional[str] = None):
    for source_path in source_directory.iterdir():
        if source_path.name == skipped_name:
            continue
        target_path = target_directory / source_path.name
        target_path.symlink_to(source_path, target_is_directory=source_path.is_dir())


def create_isolated_benchmark_root(source_root: Path, target_root: Path, tpcds_answers: Optional[Path] = None):
    """Give a runner a root directory of its own, with existing benchmark data symlinked into it."""
    target_root.mkdir()
    symlink_directory_entries(source_root, target_root, BENCHMARK_DATA_DIRECTORY)
    target_data_directory = target_root / BENCHMARK_DATA_DIRECTORY
    target_data_directory.mkdir()
    source_data_directory = source_root / BENCHMARK_DATA_DIRECTORY
    if source_data_directory.is_dir():
        symlink_directory_entries(source_data_directory, target_data_directory)
    if tpcds_answers is not None:
        source_directory = source_root
        target_directory = target_root
        for name in ("extension", "tpcds", "dsdgen"):
            source_directory = source_directory / name
            target_directory = target_directory / name
            target_directory.unlink()
            target_directory.mkdir()
            symlink_directory_entries(source_directory, target_directory)
        answers_directory = target_directory / "answers"
        answers_directory.unlink()
        answers_directory.symlink_to(tpcds_answers, target_is_directory=True)


class BenchmarkRunner:
    def __init__(
        self,
        path: str,
        label: str,
        threads: Optional[int] = None,
        memory_limit: Optional[str] = None,
        verbose: bool = False,
        disable_timeout: bool = False,
        benchmark_arguments: Optional[List[Tuple[str, str]]] = None,
        root_directory: Optional[str] = None,
    ):
        self.path = path
        self.label = label
        self.threads = threads
        self.memory_limit = memory_limit
        self.verbose = verbose
        self.disable_timeout = disable_timeout
        self.benchmark_arguments = benchmark_arguments or []
        self.root_directory = root_directory
        self.extension_directory = find_extension_directory(path)
        self.cache_directory = (
            os.path.join(root_directory, BENCHMARK_DATA_DIRECTORY)
            if root_directory
            else find_benchmark_cache_directory(path)
        )

    def run(self, benchmark: str, timed_runs: int) -> Tuple[Optional[List[float]], Optional[str]]:
        arguments = [self.path, benchmark]
        if self.threads is not None:
            arguments.append(f"--threads={self.threads}")
        if self.memory_limit is not None:
            arguments.append(f"--memory_limit={self.memory_limit}")
        if self.disable_timeout:
            arguments.append("--disable-timeout")
        if self.root_directory:
            arguments.extend(["--root-dir", self.root_directory])
        for name, value in self.benchmark_arguments:
            arguments.extend([f"--{name}", value])
        arguments.extend(["--timed-runs", str(timed_runs)])

        process_timeout = DISABLED_RUNNER_TIMEOUT if self.disable_timeout else DEFAULT_PROCESS_TIMEOUT
        process_environment = os.environ.copy()
        if self.extension_directory:
            process_environment[EXTENSION_DIRECTORY_ENV] = self.extension_directory
        try:
            process = subprocess.run(
                arguments,
                env=process_environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=process_timeout,
                text=True,
                check=False,
            )
        except subprocess.TimeoutExpired:
            message = f"Aborted after exceeding the {process_timeout}-second process limit"
            print(f"Failed to run benchmark {benchmark}: {message}", flush=True)
            return None, message

        if process.returncode != 0:
            print(f"Failed to run benchmark {benchmark}", flush=True)
            print(STDERR_HEADER, flush=True)
            print(process.stderr, flush=True)
            print(STDOUT_HEADER, flush=True)
            print(process.stdout, flush=True)
            return None, process.stderr or process.stdout or f"Benchmark runner exited with code {process.returncode}"

        if self.verbose:
            if os.getenv("CI") == "true":
                print(f"::group::raw output: {self.label} {benchmark}", flush=True)
                print(process.stderr, flush=True)
                print("::endgroup::", flush=True)
            else:
                print(process.stderr, flush=True)

        timings = []
        stderr_lines = process.stderr.splitlines()
        try:
            rows = csv.reader(stderr_lines, delimiter='\t')
            next(rows)
        except StopIteration as exception:
            message = f"Could not parse benchmark timings: {exception}"
            print(f"Failed to run benchmark {benchmark}: {message}", flush=True)
            return None, message

        for line_index, row in enumerate(rows, start=1):
            if not row:
                continue
            try:
                timings.append(float(row[2]))
            except (IndexError, ValueError):
                message = benchmark_failure_message(
                    self.label, benchmark, row, stderr_lines[line_index + 1 :], process.stdout
                )
                print(f"Failed to run benchmark {benchmark}: {message}", flush=True)
                return None, message

        if len(timings) != timed_runs:
            message = f"Expected {timed_runs} benchmark timings, received {len(timings)}"
            print(f"Failed to run benchmark {benchmark}: {message}", flush=True)
            return None, message
        return timings, None
