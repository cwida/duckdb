import math
import os
import re
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.regression.benchmark import (
    BENCHMARK_DATA_DIRECTORY,
    BenchmarkRunner,
    EXTENSION_DIRECTORY_ENV,
    create_isolated_benchmark_root,
    find_benchmark_cache_directory,
    find_extension_directory,
)
from scripts.regression.comparison import benchmark_measurement, confirmation_run_count, sampling_batch_sizes


class TestBenchmarkRunner(unittest.TestCase):
    def test_finds_artifact_local_extension_directory(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            release_directory = Path(temp_directory) / "release"
            runner_path = release_directory / "benchmark" / "benchmark_runner"
            extension_directory = release_directory / "repository" / "v1.4.0" / "linux_amd64"
            extension_directory.mkdir(parents=True)
            (extension_directory / "fts.duckdb_extension").touch()

            self.assertEqual(find_extension_directory(str(runner_path)), str(extension_directory.resolve()))

    def test_rejects_multiple_artifact_extension_directories(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            release_directory = Path(temp_directory) / "release"
            runner_path = release_directory / "benchmark" / "benchmark_runner"
            for platform in ["linux_amd64", "linux_arm64"]:
                extension_directory = release_directory / "repository" / "v1.4.0" / platform
                extension_directory.mkdir(parents=True)
                (extension_directory / "fts.duckdb_extension").touch()

            with self.assertRaisesRegex(ValueError, "multiple extension directories"):
                find_extension_directory(str(runner_path))

    def test_finds_shared_artifact_benchmark_cache_directory(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            build_directory = Path(temp_directory) / "build"
            runner_paths = [
                build_directory / "base" / "release" / "benchmark" / "benchmark_runner",
                build_directory / "current" / "release" / "benchmark" / "benchmark_runner",
            ]
            cache_directories = {find_benchmark_cache_directory(str(path)) for path in runner_paths}
            expected_directory = os.path.abspath(build_directory / "duckdb_benchmark_data")
            self.assertEqual(cache_directories, {expected_directory})

    def test_isolated_root_symlinks_existing_benchmark_data(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            source_root = Path(temp_directory) / "duckdb"
            source_data_directory = source_root / BENCHMARK_DATA_DIRECTORY
            source_data_directory.mkdir(parents=True)
            (source_root / "benchmark").mkdir()
            (source_data_directory / "real_nest.duckdb").write_text("data", encoding="utf-8")

            target_roots = [Path(temp_directory) / "base", Path(temp_directory) / "pr"]
            for target_root in target_roots:
                create_isolated_benchmark_root(source_root, target_root)
                data_directory = target_root / BENCHMARK_DATA_DIRECTORY
                self.assertTrue((target_root / "benchmark").is_symlink())
                self.assertFalse(data_directory.is_symlink())
                self.assertEqual(
                    (data_directory / "real_nest.duckdb").resolve(),
                    (source_data_directory / "real_nest.duckdb").resolve(),
                )

            # generated data stays inside the root that wrote it
            (target_roots[0] / BENCHMARK_DATA_DIRECTORY / "tpch_sf1.duckdb").write_text("base", encoding="utf-8")
            self.assertFalse((target_roots[1] / BENCHMARK_DATA_DIRECTORY / "tpch_sf1.duckdb").exists())
            self.assertFalse((source_data_directory / "tpch_sf1.duckdb").exists())

    def test_passes_artifact_extension_directory_to_runner(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            release_directory = Path(temp_directory) / "release"
            runner_path = release_directory / "benchmark" / "benchmark_runner"
            extension_directory = release_directory / "repository" / "v1.4.0" / "linux_amd64"
            extension_directory.mkdir(parents=True)
            (extension_directory / "fts.duckdb_extension").touch()
            completed_process = subprocess.CompletedProcess(
                args=[], returncode=0, stdout="", stderr="name\trun\ttiming\nquery\t1\t1.0\n"
            )

            with patch("scripts.regression.benchmark.subprocess.run", return_value=completed_process) as run:
                timings, error = BenchmarkRunner(str(runner_path), "current").run("query.benchmark", 1)

            self.assertEqual(timings, [1.0])
            self.assertIsNone(error)
            self.assertEqual(run.call_args.kwargs["env"][EXTENSION_DIRECTORY_ENV], str(extension_directory.resolve()))

    def test_base_answers_override_preserves_pr_sources_and_answers(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            source_root = Path(temp_directory) / "duckdb"
            dsdgen_directory = source_root / "extension/tpcds/dsdgen"
            pr_answers = dsdgen_directory / "answers/sf10"
            pr_answers.mkdir(parents=True)
            (pr_answers / "38.csv").write_text("487", encoding="utf-8")
            queries = dsdgen_directory / "queries"
            queries.mkdir()
            (queries / "38.sql").write_text("SELECT count(*) FROM t;", encoding="utf-8")
            base_answers = Path(temp_directory) / "base-answers"
            (base_answers / "sf10").mkdir(parents=True)
            (base_answers / "sf10/38.csv").write_text("488", encoding="utf-8")

            base_root = Path(temp_directory) / "base"
            pr_root = Path(temp_directory) / "pr"
            create_isolated_benchmark_root(source_root, base_root, base_answers)
            create_isolated_benchmark_root(source_root, pr_root)

            answer_path = "extension/tpcds/dsdgen/answers/sf10/38.csv"
            self.assertEqual((base_root / answer_path).read_text(encoding="utf-8"), "488")
            self.assertEqual((pr_root / answer_path).read_text(encoding="utf-8"), "487")
            self.assertEqual((source_root / answer_path).read_text(encoding="utf-8"), "487")
            query_path = "extension/tpcds/dsdgen/queries/38.sql"
            self.assertEqual((base_root / query_path).resolve(), (source_root / query_path).resolve())
            self.assertEqual((pr_root / query_path).resolve(), (source_root / query_path).resolve())

    def test_surfaces_incorrect_result_diagnostic(self):
        completed_process = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="",
            stderr=(
                "name\trun\ttiming\n"
                "query.benchmark\t1\tINCORRECT\n"
                "INCORRECT RESULT: Invalid Input Error: attempted to read past the end of the segment\n"
            ),
        )

        with patch("scripts.regression.benchmark.subprocess.run", return_value=completed_process):
            timings, error = BenchmarkRunner("benchmark_runner", "current").run("query.benchmark", 1)

        self.assertIsNone(timings)
        self.assertEqual(
            error,
            "current benchmark runner reported INCORRECT for query.benchmark on run 1:\n"
            "INCORRECT RESULT: Invalid Input Error: attempted to read past the end of the segment",
        )

    def test_surfaces_error_diagnostic(self):
        completed_process = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="",
            stderr="name\trun\ttiming\nquery.benchmark\t2\tERROR\nData Corruption Error: invalid segment size\n",
        )

        with patch("scripts.regression.benchmark.subprocess.run", return_value=completed_process):
            timings, error = BenchmarkRunner("benchmark_runner", "current").run("query.benchmark", 2)

        self.assertIsNone(timings)
        self.assertEqual(
            error,
            "current benchmark runner reported ERROR for query.benchmark on run 2:\n"
            "Data Corruption Error: invalid segment size",
        )

    def test_uses_stdout_failure_summary_when_stderr_has_no_diagnostic(self):
        completed_process = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="failure summary with the underlying exception\n",
            stderr="name\trun\ttiming\nquery.benchmark\t1\tINCORRECT\n",
        )

        with patch("scripts.regression.benchmark.subprocess.run", return_value=completed_process):
            timings, error = BenchmarkRunner("benchmark_runner", "current").run("query.benchmark", 1)

        self.assertIsNone(timings)
        self.assertEqual(
            error,
            "current benchmark runner reported INCORRECT for query.benchmark on run 1:\n"
            "failure summary with the underlying exception",
        )


class TestBenchmarkComparison(unittest.TestCase):
    def test_measurement_uses_medians_and_observed_ranges(self):
        measurement = benchmark_measurement([1.0, 10.0, 100.0], [0.9, 9.0, 200.0])
        self.assertEqual(measurement.old_timing, 10.0)
        self.assertEqual(measurement.old_min, 1.0)
        self.assertEqual(measurement.old_max, 100.0)
        self.assertEqual(measurement.new_timing, 9.0)
        self.assertEqual(measurement.new_min, 0.9)
        self.assertEqual(measurement.new_max, 200.0)
        self.assertAlmostEqual(measurement.ratio, 0.9)

    def test_measurement_validation(self):
        with self.assertRaises(ValueError):
            benchmark_measurement([], [])
        with self.assertRaises(ValueError):
            benchmark_measurement([1.0], [1.0, 2.0])
        with self.assertRaises(ValueError):
            benchmark_measurement([0.0], [1.0])
        with self.assertRaises(ValueError):
            benchmark_measurement([1.0], [math.inf])

    def test_confirmation_runs_scale_with_faster_side_median(self):
        cases = [(0.02, 100), (0.05, 60), (0.10, 30), (0.30, 30), (4.00, 30)]
        for timing, expected_runs in cases:
            with self.subTest(timing=timing):
                measurement = benchmark_measurement([timing] * 10, [timing * 1.2] * 10)
                self.assertEqual(confirmation_run_count(measurement), expected_runs)

    def test_sampling_batches_round_up_to_five(self):
        cases = [(1, [5]), (5, [5]), (6, [5, 5]), (12, [5, 5, 5]), (84, [5] * 17)]
        for requested_runs, expected_batches in cases:
            with self.subTest(requested_runs=requested_runs):
                self.assertEqual(sampling_batch_sizes(requested_runs), expected_batches)
        with self.assertRaises(ValueError):
            sampling_batch_sizes(0)


class TestRegressionRunnerIntegration(unittest.TestCase):
    repository_root = Path(__file__).resolve().parents[2]

    stable_runner_source = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
cache_path = os.getenv("BENCHMARK_CACHE_PATH")
expected_cache_state = os.getenv("EXPECTED_BENCHMARK_CACHE_STATE")
if expected_cache_state:
    actual_cache_state = "present" if Path(cache_path).exists() else "absent"
    if actual_cache_state != expected_cache_state:
        raise SystemExit(1)
expected_memory_limit = os.getenv("EXPECTED_MEMORY_LIMIT")
if expected_memory_limit and f"--memory_limit={expected_memory_limit}" not in sys.argv:
    raise SystemExit(1)
expected_benchmark_argument = os.getenv("EXPECTED_BENCHMARK_ARGUMENT")
if expected_benchmark_argument:
    argument_name, argument_value = expected_benchmark_argument.split("=", 1)
    argument_index = sys.argv.index(f"--{argument_name}")
    if sys.argv[argument_index + 1] != argument_value:
        raise SystemExit(1)
timing = float(os.environ["BENCHMARK_NEW_TIMING"] if label == "new" else os.environ["BENCHMARK_OLD_TIMING"])
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    print(f"{sys.argv[1]}\\t{run}\\t{timing}", file=sys.stderr)
"""

    range_runner_source = """#!/usr/bin/env python3
import os
import sys

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
values = [0.229, 0.235, 0.244, 0.255, 0.267] if label == "new" else [0.219, 0.225, 0.231, 0.240, 0.248]
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    print(f"{sys.argv[1]}\\t{run}\\t{values[(run - 1) % len(values)]}", file=sys.stderr)
"""

    aligned_range_runner_source = """#!/usr/bin/env python3
import os
import sys

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
benchmark = sys.argv[1]
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
values = {
    "q26.benchmark": {
        "old": [0.0805, 0.084, 0.0882, 0.095, 0.103],
        "new": [0.0757, 0.082, 0.085, 0.095, 0.104],
    },
    "q28.benchmark": {
        "old": [0.397, 0.420, 0.436, 0.460, 0.499],
        "new": [0.356, 0.390, 0.411, 0.440, 0.474],
    },
    "q23.benchmark": {
        "old": [1.27, 1.40, 1.62, 1.80, 2.05],
        "new": [1.23, 1.40, 1.52, 1.70, 1.82],
    },
    "q22.benchmark": {
        "old": [0.718, 0.850, 0.967, 1.10, 1.37],
        "new": [0.623, 0.700, 0.779, 0.900, 1.13],
    },
}[benchmark][label]
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    print(f"{benchmark}\\t{run}\\t{values[(run - 1) % len(values)]}", file=sys.stderr)
"""

    adaptive_early_stop_runner_source = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
counter_path = Path(os.environ["BENCHMARK_COUNTER_DIR"]) / f"{label}.count"
invocation = int(counter_path.read_text(encoding="utf-8")) if counter_path.exists() else 0
counter_path.write_text(str(invocation + 1), encoding="utf-8")
timing = 1.04 if label == "new" and invocation < 2 else 1.0
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    print(f"{sys.argv[1]}\\t{run}\\t{timing}", file=sys.stderr)
"""

    early_stop_after_twenty_runner_source = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
counter_path = Path(os.environ["BENCHMARK_COUNTER_DIR"]) / f"{label}.count"
invocation = int(counter_path.read_text(encoding="utf-8")) if counter_path.exists() else 0
counter_path.write_text(str(invocation + 1), encoding="utf-8")
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    if label == "old":
        timing = 1.0
    elif invocation < 2:
        timing = 1.04
    elif invocation < 4 and run <= 3:
        timing = 1.1
    else:
        timing = 1.0
    print(f"{sys.argv[1]}\\t{run}\\t{timing}", file=sys.stderr)
"""

    multi_query_runner_source = """#!/usr/bin/env python3
import os
import sys

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
benchmark = sys.argv[1]
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
timing = 1.1 if label == "new" and benchmark == "q1.benchmark" else 1.0
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    print(f"{benchmark}\\t{run}\\t{timing}", file=sys.stderr)
"""

    malformed_runner_source = """#!/usr/bin/env python3
import sys

print("name\\trun\\ttiming", file=sys.stderr)
print("not-a-valid-timing-row", file=sys.stderr)
"""

    incorrect_runner_source = """#!/usr/bin/env python3
import sys

print("name\\trun\\ttiming", file=sys.stderr)
print(f"{sys.argv[1]}\\t1\\tINCORRECT", file=sys.stderr)
print("INCORRECT RESULT: Data Corruption Error: attempted to read past the end of the segment", file=sys.stderr)
"""

    tpcds_answers_runner_source = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

label = os.path.basename(sys.argv[0])
root = Path(sys.argv[sys.argv.index("--root-dir") + 1])
expected = (root / "extension/tpcds/dsdgen/answers/sf10/38.csv").read_text().splitlines()[1]
actual = "488" if label == "old" else "487"
print("name\\trun\\ttiming", file=sys.stderr)
if expected != actual:
    print(f"{sys.argv[1]}\\t1\\tINCORRECT", file=sys.stderr)
    print(f"INCORRECT RESULT: expected {expected}, got {actual}", file=sys.stderr)
else:
    runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
    for run in range(1, runs + 1):
        print(f"{sys.argv[1]}\\t{run}\\t1.0", file=sys.stderr)
"""

    benchmark_data_runner_source = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
state_directory = Path(sys.argv[sys.argv.index("--root-dir") + 1]) / "duckdb_benchmark_data"
owner_path = state_directory / "shared-owner"
previous_owner = owner_path.read_text(encoding="utf-8") if owner_path.exists() else None
owner_path.write_text(label, encoding="utf-8")
print("name\\trun\\timing", file=sys.stderr)
if label == "old" and previous_owner == "new":
    print(f"{sys.argv[1]}\\t1\\tINCORRECT", file=sys.stderr)
    print("INCORRECT RESULT: old runner opened state written by new runner", file=sys.stderr)
else:
    for run in range(1, runs + 1):
        print(f"{sys.argv[1]}\\t{run}\\t1.0", file=sys.stderr)
"""

    external_state_runner_source = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

label = os.path.basename(sys.argv[0])
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
owner_path = Path(os.environ["BENCHMARK_COUNTER_DIR"]) / "external-owner"
previous_owner = owner_path.read_text(encoding="utf-8") if owner_path.exists() else None
owner_path.write_text(label, encoding="utf-8")
print("name\\trun\\timing", file=sys.stderr)
if label == "old" and previous_owner == "new":
    print(f"{sys.argv[1]}\\t1\\tINCORRECT", file=sys.stderr)
    print("INCORRECT RESULT: old runner opened state written by new runner", file=sys.stderr)
else:
    for run in range(1, runs + 1):
        print(f"{sys.argv[1]}\\t{run}\\t1.0", file=sys.stderr)
"""

    missing_base_runner_source = """#!/usr/bin/env python3
import os
import sys

label = os.path.basename(sys.argv[0])
benchmark = sys.argv[1]
runs = int(sys.argv[sys.argv.index("--timed-runs") + 1])
with open(os.environ["BENCHMARK_ORDER_LOG"], "a", encoding="utf-8") as order_log:
    order_log.write(f"{label}:{runs}\\n")
base_missing = benchmark in ("new_query.benchmark", "broken_query.benchmark", "missing_everywhere.benchmark")
if label == "old" and base_missing:
    print("Benchmark to run could not be found.", file=sys.stderr)
    raise SystemExit(1)
if label == "old" and benchmark == "base_error.benchmark":
    print("Base benchmark setup failed.", file=sys.stderr)
    raise SystemExit(1)
if label == "new" and benchmark == "broken_query.benchmark":
    print("PR benchmark setup failed.", file=sys.stderr)
    raise SystemExit(1)
if label == "new" and benchmark == "missing_everywhere.benchmark":
    print("Benchmark to run could not be found.", file=sys.stderr)
    raise SystemExit(1)
print("name\\trun\\ttiming", file=sys.stderr)
for run in range(1, runs + 1):
    print(f"{benchmark}\\t{run}\\t1.0", file=sys.stderr)
"""

    def run_regression_test(
        self,
        runner_source,
        new_timing="1.0",
        old_timing="1.0",
        extra_args=None,
        ci=False,
        benchmarks=None,
        create_cache=False,
        expected_cache_state=None,
        expected_memory_limit=None,
        expected_benchmark_argument=None,
        step_summary=False,
        old_tpcds_answer=None,
    ):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            runner_paths = {
                "old": temp_path / "build" / "base" / "release" / "benchmark" / "old",
                "new": temp_path / "build" / "current" / "release" / "benchmark" / "new",
            }
            for runner_path in runner_paths.values():
                runner_path.parent.mkdir(parents=True, exist_ok=True)
                runner_path.write_text(runner_source, encoding="utf-8")
                runner_path.chmod(runner_path.stat().st_mode | stat.S_IXUSR)
            benchmark_list = temp_path / "benchmarks.csv"
            benchmark_list.write_text("\n".join(benchmarks or ["fake.benchmark"]) + "\n", encoding="utf-8")
            order_log = temp_path / "order.log"
            summary_path = temp_path / "summary.md"
            env = os.environ.copy()
            env["BENCHMARK_ORDER_LOG"] = str(order_log)
            env["BENCHMARK_COUNTER_DIR"] = str(temp_path)
            env["BENCHMARK_NEW_TIMING"] = new_timing
            env["BENCHMARK_OLD_TIMING"] = old_timing
            if create_cache:
                cache_path = temp_path / "build" / "duckdb_benchmark_data"
                cache_path.mkdir()
                (cache_path / "marker").write_text("cached", encoding="utf-8")
                env["BENCHMARK_CACHE_PATH"] = str(cache_path)
            if expected_cache_state:
                env["EXPECTED_BENCHMARK_CACHE_STATE"] = expected_cache_state
            if expected_memory_limit:
                env["EXPECTED_MEMORY_LIMIT"] = expected_memory_limit
            if expected_benchmark_argument:
                env["EXPECTED_BENCHMARK_ARGUMENT"] = expected_benchmark_argument
            if ci:
                env["CI"] = "true"
            if step_summary:
                env["GITHUB_STEP_SUMMARY"] = str(summary_path)

            command = [
                sys.executable,
                str(self.repository_root / "scripts/regression/test_runner.py"),
                "--old",
                str(runner_paths["old"]),
                "--new",
                str(runner_paths["new"]),
                "--benchmarks",
                str(benchmark_list),
                "--verbose",
            ]
            if extra_args:
                command.extend(extra_args)
            if old_tpcds_answer is not None:
                answers = temp_path / "base-answers"
                (answers / "sf10").mkdir(parents=True)
                (answers / "sf10/38.csv").write_text(f"count_star()\n{old_tpcds_answer}\n", encoding="utf-8")
                command.extend(["--old-tpcds-answers", str(answers)])
            process = subprocess.run(
                command,
                cwd=self.repository_root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            order = order_log.read_text(encoding="utf-8").splitlines() if order_log.exists() else []
            summary = summary_path.read_text(encoding="utf-8") if summary_path.exists() else ""
            return process, order, summary

    def test_tpcds_generator_change_uses_each_revisions_answers(self):
        for base_answer, returncode in [(None, 1), ("488", 0), ("486", 1)]:
            with self.subTest(base_answer=base_answer):
                process, _, _ = self.run_regression_test(
                    self.tpcds_answers_runner_source,
                    extra_args=["--samples", "5"],
                    old_tpcds_answer=base_answer,
                )
                self.assertEqual(process.returncode, returncode, process.stdout + process.stderr)
                if returncode:
                    self.assertIn("INCORRECT RESULT:", process.stdout)

    def test_tpcds_answer_override_still_rejects_incorrect_pr_results(self):
        process, _, _ = self.run_regression_test(
            self.tpcds_answers_runner_source.replace('else "487"', 'else "486"'),
            extra_args=["--samples", "5"],
            old_tpcds_answer="488",
        )
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertIn("INCORRECT RESULT: expected 487, got 486", process.stdout)

    def test_missing_base_tpcds_answers_are_rejected(self):
        process, _, _ = self.run_regression_test(
            self.stable_runner_source,
            extra_args=[
                "--old-tpcds-answers",
                str(self.repository_root / "missing-tpcds-answers"),
            ],
        )
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertIn("Failed to find base TPC-DS answers directory", process.stdout)

    @staticmethod
    def expected_order(total_runs):
        order = []
        for batch_index, batch_size in enumerate(sampling_batch_sizes(total_runs)):
            if batch_index % 2 == 0:
                order.extend([f"old:{batch_size}", f"new:{batch_size}"])
            else:
                order.extend([f"new:{batch_size}", f"old:{batch_size}"])
        return order

    def test_stable_adaptive_query_stops_after_initial_samples(self):
        process, order, _ = self.run_regression_test(self.stable_runner_source)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(10))
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertIn("sampling: adaptive; 10 initial pairs, then 30–100 confirmation pairs outside ±3%", plain_output)
        self.assertIn("query regression: median change ≥ +10.0% (warning)", plain_output)
        self.assertIn("CI failure: geomean change ≥ +10.0% or ≥ +50.0 ms", plain_output)
        self.assertNotIn("confidence", plain_output.lower())
        self.assertNotIn("UNCERTAIN", plain_output)
        self.assertIn("UNCHANGED (±3%)\n1 benchmarks", plain_output)
        self.assertTrue(plain_output.rstrip().endswith("result: passed; no query regressions"))

    def test_noise_boundaries_are_inclusive(self):
        for new_timing in ("0.97", "1.03"):
            with self.subTest(new_timing=new_timing):
                process, order, _ = self.run_regression_test(self.stable_runner_source, new_timing=new_timing)
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                self.assertEqual(order, self.expected_order(10))
                plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
                self.assertIn("UNCHANGED (±3%)", plain_output)

    def test_adaptive_batches_alternate_and_full_budget_is_default(self):
        process, order, _ = self.run_regression_test(self.stable_runner_source, new_timing="1.08")
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(40))
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertIn("confirm: fake.benchmark: 30 pairs", plain_output)
        self.assertRegex(plain_output, r"(?m)^fake .*\s40$")
        self.assertNotIn("10+30", plain_output)
        self.assertIn("result: failed; geomean regression; no query regressions", plain_output)

    def test_adaptive_early_stop_after_ten_confirmation_samples(self):
        process, order, _ = self.run_regression_test(
            self.adaptive_early_stop_runner_source, extra_args=["--early-stop"]
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(20))
        self.assertIn(
            "confirm: fake.benchmark: 10 pairs | median change +0.0% | within ±3% (stopped early)",
            process.stdout,
        )

    def test_adaptive_early_stop_after_twenty_confirmation_samples(self):
        process, order, _ = self.run_regression_test(
            self.early_stop_after_twenty_runner_source, extra_args=["--early-stop"]
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(30))
        self.assertIn(
            "confirm: fake.benchmark: 20 pairs | median change +0.0% | within ±3% (stopped early)",
            process.stdout,
        )

    def test_fixed_samples_round_up_and_run_every_sample(self):
        process, order, _ = self.run_regression_test(self.stable_runner_source, extra_args=["--samples", "12"])
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(15))
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertIn("sampling: fixed; 15 samples per binary (--samples=12 rounded up)", plain_output)
        self.assertIn("geomean: 1.0 s -> 1.0 s", plain_output)
        self.assertIn("(15 samples)", plain_output)

    def test_fixed_samples_can_early_stop(self):
        process, order, _ = self.run_regression_test(
            self.stable_runner_source, extra_args=["--samples", "30", "--early-stop"]
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(10))
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertIn("sampling: fixed; 30 samples per binary (--samples=30); median early stop", plain_output)
        self.assertIn("(up to 30 samples)", plain_output)

    def test_compact_table_shows_gray_observed_ranges(self):
        process, _, _ = self.run_regression_test(self.range_runner_source, extra_args=["--samples", "5"])
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertIn("benchmark  base median", plain_output)
        self.assertIn(
            "fake       231.0 ms [219.0 ms…248.0 ms]  244.0 ms [229.0 ms…267.0 ms]  +13.0 ms (+5.6%)",
            plain_output,
        )
        self.assertIn("231.0 ms \033[90m[219.0 ms…248.0 ms]\033[0m", process.stdout)
        self.assertIn("244.0 ms \033[90m[229.0 ms…267.0 ms]\033[0m", process.stdout)

    def test_table_aligns_mixed_unit_ranges(self):
        benchmarks = ["q26.benchmark", "q28.benchmark", "q23.benchmark", "q22.benchmark"]
        process, _, _ = self.run_regression_test(
            self.aligned_range_runner_source,
            extra_args=["--samples", "5"],
            benchmarks=benchmarks,
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        expected_ranges = {
            "q26": "[ 80.5 ms…103.0 ms]",
            "q28": "[397.0 ms…499.0 ms]",
            "q23": "[  1.3 s …  2.0 s ]",
            "q22": "[718.0 ms…  1.4 s ]",
        }
        rows = {}
        for line in plain_output.splitlines():
            name = line.split(maxsplit=1)[0] if line else ""
            if name in expected_ranges:
                rows[name] = line
        self.assertEqual(set(rows), set(expected_ranges))
        for name, expected_range in expected_ranges.items():
            self.assertIn(expected_range, rows[name])
        ellipsis_positions = {tuple(match.start() for match in re.finditer("…", row)) for row in rows.values()}
        self.assertEqual(len(ellipsis_positions), 1)
        range_decimal_positions = set()
        for row in rows.values():
            base_start = row.index("[")
            base_ellipsis = row.index("…", base_start)
            pr_start = row.index("[", base_ellipsis)
            pr_ellipsis = row.index("…", pr_start)
            range_decimal_positions.add(
                (
                    row.index(".", base_start),
                    row.index(".", base_ellipsis),
                    row.index(".", pr_start),
                    row.index(".", pr_ellipsis),
                )
            )
        self.assertEqual(len(range_decimal_positions), 1)

    def test_exact_ten_percent_query_regression_warns_but_suite_can_pass(self):
        benchmarks = ["q1.benchmark", "q2.benchmark", "q3.benchmark", "q4.benchmark", "q5.benchmark"]
        process, _, summary = self.run_regression_test(
            self.multi_query_runner_source,
            ci=True,
            benchmarks=benchmarks,
            step_summary=True,
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertIn("::warning title=Benchmark query regression::", plain_output)
        self.assertIn("REGRESSIONS (≥+10%)", plain_output)
        self.assertIn("q1", plain_output)
        self.assertTrue(plain_output.rstrip().endswith("result: passed; 1 query regression"))
        self.assertIn("## Query Regressions", summary)
        self.assertIn("| Benchmark | Base median | PR median | Delta median | Runs |", summary)
        self.assertIn("| `q1.benchmark` | `1.0 s [1.0 s…1.0 s]` | `1.1 s [1.1 s…1.1 s]`", summary)
        self.assertIn("| `40` |", summary)
        self.assertNotIn("confidence", summary.lower())

    def test_geomean_relative_threshold_fails_ci(self):
        process, _, _ = self.run_regression_test(self.stable_runner_source, old_timing="1", new_timing="1.1", ci=True)
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertIn("::error title=Geomean benchmark regression::", process.stdout)
        self.assertIn("result: \033[31mfailed; geomean regression\033[0m", process.stdout)

    def test_geomean_absolute_threshold_fails_ci(self):
        process, _, _ = self.run_regression_test(self.stable_runner_source, old_timing="1", new_timing="1.05", ci=True)
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertIn("::error title=Geomean benchmark regression::", process.stdout)
        self.assertIn("+50.0 ms (+5.0%)", process.stdout)

    def test_missing_base_benchmark_warns_and_smoke_tests_pr_once(self):
        process, order, summary = self.run_regression_test(
            self.missing_base_runner_source,
            ci=True,
            benchmarks=["new_query.benchmark"],
            step_summary=True,
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, ["old:5", "new:1"])
        self.assertIn("::warning title=Benchmark missing from Linux Base::", process.stdout)
        self.assertIn("SKIPPED (missing from Base)", process.stdout)
        self.assertIn("new_query: PR smoke test passed", process.stdout)
        self.assertIn("geomean: unavailable", process.stdout)
        self.assertIn("1 benchmark skipped (missing from Base)", process.stdout)
        self.assertIn("## Benchmarks Missing From Base: `benchmarks`", summary)
        self.assertIn("| `new_query.benchmark` | passed |", summary)

    def test_missing_base_query_does_not_exclude_supported_queries(self):
        process, order, _ = self.run_regression_test(
            self.missing_base_runner_source,
            benchmarks=["stable.benchmark", "new_query.benchmark"],
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, ["old:5", "new:5", "new:5", "old:5", "old:5", "new:1"])
        self.assertIn("UNCHANGED (±3%)", process.stdout)
        self.assertIn("geomean: 1.0 s -> 1.0 s", process.stdout)
        self.assertIn("new_query: PR smoke test passed", process.stdout)

    def test_missing_base_benchmark_does_not_hide_pr_failure(self):
        for benchmark in ("broken_query.benchmark", "missing_everywhere.benchmark"):
            with self.subTest(benchmark=benchmark):
                process, order, _ = self.run_regression_test(
                    self.missing_base_runner_source,
                    ci=True,
                    benchmarks=[benchmark],
                )
                self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
                self.assertEqual(order, ["old:5", "new:1"])
                self.assertIn("::error title=Regression benchmark failure::", process.stdout)
                self.assertNotIn("::warning title=Benchmark missing from Linux Base::", process.stdout)

    def test_other_base_errors_still_fail(self):
        process, order, _ = self.run_regression_test(
            self.missing_base_runner_source,
            benchmarks=["base_error.benchmark"],
        )
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertEqual(order, ["old:5", "new:5"])
        self.assertIn("Base benchmark setup failed.", process.stdout)
        self.assertIn("benchmark failure", process.stdout)

    def test_nofail_suppresses_only_geomean_gate(self):
        process, _, _ = self.run_regression_test(
            self.stable_runner_source, old_timing="1", new_timing="1.1", ci=True, extra_args=["--nofail"]
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertIn("::warning title=Geomean benchmark regression::", process.stdout)
        self.assertIn("result: \033[32mpassed (--nofail)\033[0m", process.stdout)

        process, _, _ = self.run_regression_test(self.malformed_runner_source, extra_args=["--nofail"])
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertIn("benchmark failure", process.stdout)

    def test_invalid_samples_are_rejected(self):
        for samples in ("0", "-1"):
            with self.subTest(samples=samples):
                process, order, _ = self.run_regression_test(
                    self.stable_runner_source, extra_args=["--samples", samples]
                )
                self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
                self.assertIn("must be greater than zero", process.stderr)
                self.assertEqual(order, [])

    def test_benchmark_cache_memory_limit_and_custom_arguments_are_preserved(self):
        process, _, _ = self.run_regression_test(
            self.stable_runner_source,
            extra_args=[
                "--memory-limit",
                "512MB",
                "--benchmark-argument",
                "sf=10",
            ],
            create_cache=True,
            expected_cache_state="present",
            expected_memory_limit="512MB",
            expected_benchmark_argument="sf=10",
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

    def test_invalid_benchmark_argument_is_rejected(self):
        for argument in ("sf", "=10", "sf="):
            with self.subTest(argument=argument):
                process, order, _ = self.run_regression_test(
                    self.stable_runner_source, extra_args=["--benchmark-argument", argument]
                )
                self.assertEqual(process.returncode, 2, process.stdout + process.stderr)
                self.assertIn("must use NAME=VALUE", process.stderr)
                self.assertEqual(order, [])

    def test_failure_details_precede_final_result(self):
        process, _, _ = self.run_regression_test(self.malformed_runner_source)
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        plain_output = re.sub(r"\x1b\[[0-9;]*m", "", process.stdout)
        self.assertLess(plain_output.index("FAILURES SUMMARY"), plain_output.index("geomean:"))
        self.assertTrue(plain_output.rstrip().endswith("result: failed; benchmark failure; no query regressions"))

    def test_incorrect_result_diagnostic_reaches_failure_summary(self):
        process, _, _ = self.run_regression_test(self.incorrect_runner_source)
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertIn(
            "INCORRECT RESULT: Data Corruption Error: attempted to read past the end of the segment",
            process.stdout,
        )
        self.assertNotIn("could not convert string to float", process.stdout)

    def test_failure_identifies_runner_that_ran_before_the_failing_runner(self):
        process, order, _ = self.run_regression_test(self.external_state_runner_source)
        self.assertEqual(process.returncode, 1, process.stdout + process.stderr)
        self.assertEqual(order, ["old:5", "new:5", "new:5", "old:5"])
        self.assertIn("Base benchmark runner reported INCORRECT", process.stdout)
        self.assertIn("Comparison batch 2 ran PR immediately before Base.", process.stdout)
        self.assertNotIn("Both runners use the same benchmark cache", process.stdout)
        self.assertIn("PR:\n No failure", process.stdout)

    def test_benchmark_directories_isolate_runner_writable_state(self):
        process, order, _ = self.run_regression_test(self.benchmark_data_runner_source)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(order, self.expected_order(10))
        self.assertNotIn("opened state written by", process.stdout)
        self.assertIn("benchmark cache: isolated Base and PR directories", process.stdout)


if __name__ == "__main__":
    unittest.main()
