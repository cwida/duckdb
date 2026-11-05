import argparse
import functools
import math
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from benchmark import BenchmarkRunner, benchmark_not_found, create_isolated_benchmark_root
from comparison import (
    MAX_CONFIRMATION_RUNS,
    MIN_CONFIRMATION_RUNS,
    SAMPLE_BATCH_SIZE,
    BenchmarkMeasurement,
    benchmark_measurement,
    confirmation_run_count,
    sampling_batch_sizes,
)


print = functools.partial(print, flush=True)

INITIAL_RUNS = 2 * SAMPLE_BATCH_SIZE
REGRESSION_LIMIT = 1.10
NOISE_THRESHOLD = 0.03
GEOMEAN_REGRESSION_SECONDS = 0.050

ANSI_RED = "\033[31m"
ANSI_GREEN = "\033[32m"
ANSI_YELLOW = "\033[33m"
ANSI_GRAY = "\033[90m"
ANSI_RESET = "\033[0m"

BUCKET_UNCHANGED = "unchanged"
BUCKET_FASTER = "faster"
BUCKET_SLOWER = "slower"
BUCKET_REGRESSION = "regression"
BUCKET_FAILURE = "failure"

OUTCOME_NO_REGRESSION = "no_regression"
OUTCOME_REGRESSION = "regression"
OUTCOME_FAILURE = "failure"
OUTCOME_MISSING_BASE = "missing_base"


@dataclass
class BenchmarkResult:
    benchmark: str
    initial_measurement: Optional[BenchmarkMeasurement] = None
    measurement: Optional[BenchmarkMeasurement] = None
    old_failure: Optional[str] = None
    new_failure: Optional[str] = None
    initial_runs: int = 0
    confirmation_runs: int = 0
    outcome: str = OUTCOME_NO_REGRESSION


@dataclass
class FormattedDuration:
    value: str
    unit: str


@dataclass
class BenchmarkRow:
    display_name: str
    old_timing: Optional[FormattedDuration]
    old_min: Optional[FormattedDuration]
    old_max: Optional[FormattedDuration]
    new_timing: Optional[FormattedDuration]
    new_min: Optional[FormattedDuration]
    new_max: Optional[FormattedDuration]
    delta: Optional[FormattedDuration]
    delta_percentage: str
    runs: str
    bucket: str
    percentage: float = 0


@dataclass
class TableLayout:
    benchmark_width: int
    timing_number_width: int
    timing_unit_width: int
    timing_width: int
    delta_number_width: int
    delta_unit_width: int
    delta_percentage_width: int
    delta_width: int
    runs_width: int


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def benchmark_argument(value: str) -> Tuple[str, str]:
    name, separator, argument_value = value.partition("=")
    if not separator or not name or not argument_value or name.startswith("-"):
        raise argparse.ArgumentTypeError("must use NAME=VALUE with a non-empty name and value")
    return name, argument_value


def parse_arguments():
    parser = argparse.ArgumentParser(description="Compare benchmarks from base and PR benchmark runners.")
    parser.add_argument("--old", required=True, help="Path to the base benchmark runner.")
    parser.add_argument("--new", required=True, help="Path to the PR benchmark runner.")
    parser.add_argument("--old-tpcds-answers", help="Base revision's TPC-DS answers directory.")
    parser.add_argument("--benchmarks", required=True, help="Path to the benchmark list.")
    parser.add_argument("--threads", type=int, help="Number of threads used by each benchmark runner.")
    parser.add_argument("--memory-limit", help="Memory limit used by each benchmark runner.")
    parser.add_argument(
        "--samples", type=positive_integer, help="Fixed samples per binary; rounded up to a batch of five."
    )
    parser.add_argument(
        "--early-stop", action="store_true", help="Stop after a ten-sample checkpoint within ±3 percent."
    )
    parser.add_argument("--verbose", action="store_true", help="Print raw benchmark runner output.")
    parser.add_argument("--nofail", action="store_true", help="Report a geomean regression without failing.")
    parser.add_argument("--disable-timeout", action="store_true", help="Disable the benchmark runner timeout.")
    parser.add_argument(
        "--benchmark-argument",
        action="append",
        type=benchmark_argument,
        default=[],
        help="Benchmark runner argument in NAME=VALUE form; may be specified more than once.",
    )
    return parser.parse_args()


def within_noise(measurement: BenchmarkMeasurement) -> bool:
    return 1.0 - NOISE_THRESHOLD <= measurement.ratio <= 1.0 + NOISE_THRESHOLD


def measurement_outcome(measurement: BenchmarkMeasurement) -> str:
    return OUTCOME_REGRESSION if measurement.ratio >= REGRESSION_LIMIT else OUTCOME_NO_REGRESSION


def run_paired_samples(
    old_runner: BenchmarkRunner,
    new_runner: BenchmarkRunner,
    benchmark: str,
    requested_runs: int,
    initial_batch_index: int,
    early_stop: bool = False,
):
    old_timings = []
    new_timings = []
    batch_index = initial_batch_index
    batch_sizes = sampling_batch_sizes(requested_runs)
    planned_runs = sum(batch_sizes)
    for batch_size in batch_sizes:
        if batch_index % 2 == 0:
            first_runner = old_runner
            second_runner = new_runner
        else:
            first_runner = new_runner
            second_runner = old_runner

        first_batch, first_failure = first_runner.run(benchmark, batch_size)
        second_runs = 1 if first_runner is old_runner and benchmark_not_found(first_failure) else batch_size
        second_batch, second_failure = second_runner.run(benchmark, second_runs)
        if first_runner is old_runner:
            old_batch, old_failure = first_batch, first_failure
            new_batch, new_failure = second_batch, second_failure
        else:
            new_batch, new_failure = first_batch, first_failure
            old_batch, old_failure = second_batch, second_failure

        if second_failure:
            second_failure += f"\nComparison batch {batch_index + 1} ran {first_runner.label} immediately before "
            second_failure += f"{second_runner.label}."
            if second_runner is old_runner:
                old_failure = second_failure
            else:
                new_failure = second_failure

        old_batch = old_batch or []
        new_batch = new_batch or []
        if benchmark_not_found(old_failure) and not new_failure:
            new_timings.extend(new_batch)
            return old_timings, new_timings, old_failure, None, batch_index, False
        if old_failure or new_failure:
            return old_timings, new_timings, old_failure, new_failure, batch_index, False
        if len(old_batch) != len(new_batch):
            failure = f"Paired benchmark batches produced different run counts: {len(old_batch)} and {len(new_batch)}"
            return old_timings, new_timings, failure, failure, batch_index, False

        old_timings.extend(old_batch)
        new_timings.extend(new_batch)
        batch_index += 1
        completed_runs = len(old_timings)
        if early_stop and completed_runs < planned_runs and completed_runs % (2 * SAMPLE_BATCH_SIZE) == 0:
            if within_noise(benchmark_measurement(old_timings, new_timings)):
                return old_timings, new_timings, None, None, batch_index, True

    return old_timings, new_timings, None, None, batch_index, False


def failed_benchmark_result(
    benchmark: str,
    old_failure: Optional[str],
    new_failure: Optional[str],
    initial_measurement: Optional[BenchmarkMeasurement],
    initial_runs: int,
    confirmation_runs: int,
) -> BenchmarkResult:
    return BenchmarkResult(
        benchmark=benchmark,
        initial_measurement=initial_measurement,
        old_failure=old_failure,
        new_failure=new_failure,
        initial_runs=initial_runs,
        confirmation_runs=confirmation_runs,
        outcome=OUTCOME_FAILURE,
    )


def missing_base_benchmark_result(benchmark: str, old_failure: Optional[str], smoke_test_runs: int) -> BenchmarkResult:
    return BenchmarkResult(
        benchmark=benchmark,
        old_failure=old_failure,
        initial_runs=smoke_test_runs,
        outcome=OUTCOME_MISSING_BASE,
    )


def measurement_status(measurement: BenchmarkMeasurement, stopped_early: bool = False) -> str:
    if stopped_early:
        return "within ±3% (stopped early)"
    if measurement.ratio >= REGRESSION_LIMIT:
        return "regression"
    if measurement.ratio > 1.0 + NOISE_THRESHOLD:
        return "slower"
    if measurement.ratio < 1.0 - NOISE_THRESHOLD:
        return "faster"
    return "within ±3%; no change"


def run_fixed_benchmark(
    old_runner: BenchmarkRunner,
    new_runner: BenchmarkRunner,
    benchmark: str,
    samples: int,
    early_stop: bool,
    verbose: bool,
) -> BenchmarkResult:
    old_timings, new_timings, old_failure, new_failure, _, stopped_early = run_paired_samples(
        old_runner, new_runner, benchmark, samples, 0, early_stop
    )
    run_count = min(len(old_timings), len(new_timings))
    if old_failure or new_failure:
        if benchmark_not_found(old_failure) and not new_failure:
            return missing_base_benchmark_result(benchmark, old_failure, len(new_timings))
        return failed_benchmark_result(benchmark, old_failure, new_failure, None, run_count, 0)

    measurement = benchmark_measurement(old_timings, new_timings)
    if verbose:
        print(
            f"samples: {benchmark}: {run_count} pairs | "
            f"median change {format_ratio_change(measurement.ratio)} | {measurement_status(measurement, stopped_early)}"
        )
    return BenchmarkResult(
        benchmark=benchmark,
        initial_measurement=measurement,
        measurement=measurement,
        initial_runs=run_count,
        outcome=measurement_outcome(measurement),
    )


def run_adaptive_benchmark(
    old_runner: BenchmarkRunner,
    new_runner: BenchmarkRunner,
    benchmark: str,
    early_stop: bool,
    verbose: bool,
) -> BenchmarkResult:
    old_initial, new_initial, old_failure, new_failure, batch_index, _ = run_paired_samples(
        old_runner, new_runner, benchmark, INITIAL_RUNS, 0
    )
    initial_count = min(len(old_initial), len(new_initial))
    if old_failure or new_failure:
        if benchmark_not_found(old_failure) and not new_failure:
            return missing_base_benchmark_result(benchmark, old_failure, len(new_initial))
        return failed_benchmark_result(benchmark, old_failure, new_failure, None, initial_count, 0)

    initial_measurement = benchmark_measurement(old_initial, new_initial)
    is_candidate = not within_noise(initial_measurement)
    requested_confirmation_runs = confirmation_run_count(initial_measurement) if is_candidate else 0
    if verbose:
        decision = f"confirming with {requested_confirmation_runs} pairs" if is_candidate else "within ±3%; done"
        print(
            f"initial: {benchmark}: {initial_count} pairs | "
            f"median change {format_ratio_change(initial_measurement.ratio)} | {decision}"
        )

    if not is_candidate:
        return BenchmarkResult(
            benchmark=benchmark,
            initial_measurement=initial_measurement,
            measurement=initial_measurement,
            initial_runs=initial_count,
            outcome=measurement_outcome(initial_measurement),
        )

    old_confirmation, new_confirmation, old_failure, new_failure, _, stopped_early = run_paired_samples(
        old_runner,
        new_runner,
        benchmark,
        requested_confirmation_runs,
        batch_index,
        early_stop,
    )
    confirmation_count = min(len(old_confirmation), len(new_confirmation))
    if old_failure or new_failure:
        return failed_benchmark_result(
            benchmark,
            old_failure,
            new_failure,
            initial_measurement,
            initial_count,
            confirmation_count,
        )

    measurement = benchmark_measurement(old_confirmation, new_confirmation)
    if verbose:
        print(
            f"confirm: {benchmark}: {confirmation_count} pairs | "
            f"median change {format_ratio_change(measurement.ratio)} | {measurement_status(measurement, stopped_early)}"
        )
    return BenchmarkResult(
        benchmark=benchmark,
        initial_measurement=initial_measurement,
        measurement=measurement,
        initial_runs=initial_count,
        confirmation_runs=confirmation_count,
        outcome=measurement_outcome(measurement),
    )


def in_ci() -> bool:
    return os.getenv("CI") == "true"


def append_step_summary(lines: List[str]):
    summary_path = os.getenv("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    with open(summary_path, "a", encoding="utf-8") as summary_file:
        summary_file.write("\n".join(lines))
        summary_file.write("\n")


def format_percentage(value: float) -> str:
    if math.isfinite(value):
        return f"{value:+.1f}%"
    return "-inf%" if value < 0 else "+inf%"


def ratio_change(ratio: float) -> float:
    return (ratio - 1.0) * 100.0


def format_ratio_change(ratio: float) -> str:
    return format_percentage(ratio_change(ratio))


def duration_unit(value: float):
    value = abs(value)
    if value >= 1.0:
        return 1.0, "s"
    if value >= 0.001:
        return 1000.0, "ms"
    if value >= 0.000001:
        return 1000000.0, "µs"
    return 1000000000.0, "ns"


def format_duration_parts(value: float, unit=None, signed: bool = False) -> FormattedDuration:
    scale, suffix = unit or duration_unit(value)
    scaled = value * scale
    if signed and scaled > 0:
        prefix = "+"
    else:
        prefix = ""
    return FormattedDuration(f"{prefix}{scaled:.1f}", suffix)


def format_duration(value: float, unit=None, signed: bool = False) -> str:
    duration = format_duration_parts(value, unit, signed)
    return f"{duration.value} {duration.unit}"


def timing_parts(timing: float, minimum: float, maximum: float) -> Tuple[str, str]:
    return format_duration(timing), f"[{format_duration(minimum)}…{format_duration(maximum)}]"


def delta_parts(measurement: BenchmarkMeasurement) -> Tuple[FormattedDuration, str]:
    delta = measurement.new_timing - measurement.old_timing
    unit = duration_unit(delta) if delta else duration_unit(measurement.old_timing)
    return format_duration_parts(delta, unit, signed=True), format_ratio_change(measurement.ratio)


def delta_text(measurement: BenchmarkMeasurement) -> str:
    delta, percentage = delta_parts(measurement)
    return f"{delta.value} {delta.unit} ({percentage})"


def gray(value: str) -> str:
    return f"{ANSI_GRAY}{value}{ANSI_RESET}"


def color_change(bucket: str, value: str) -> str:
    if bucket in (BUCKET_REGRESSION, BUCKET_SLOWER, BUCKET_FAILURE):
        return f"{ANSI_RED}{value}{ANSI_RESET}"
    if bucket == BUCKET_FASTER:
        return f"{ANSI_GREEN}{value}{ANSI_RESET}"
    return f"{ANSI_GRAY}{value}{ANSI_RESET}"


def emit_github_error(title: str, message: str):
    if in_ci():
        print(f"::error title={title}::{message}")


def emit_github_warning(title: str, message: str):
    if in_ci():
        print(f"::warning title={title}::{message}")


def benchmark_common_prefix(benchmarks: List[str]) -> str:
    directories = [os.path.dirname(benchmark) for benchmark in benchmarks if os.path.dirname(benchmark)]
    if not directories:
        return ""
    common_prefix = os.path.commonpath(directories)
    if common_prefix in ("", "."):
        return ""
    return common_prefix + os.sep


def benchmark_display_names(benchmarks: List[str]) -> Dict[str, str]:
    common_prefix = benchmark_common_prefix(benchmarks)
    display_names = {}
    for benchmark in benchmarks:
        display_name = (
            benchmark[len(common_prefix) :] if common_prefix and benchmark.startswith(common_prefix) else benchmark
        )
        if display_name.endswith(".benchmark"):
            display_name = display_name[: -len(".benchmark")]
        display_names[benchmark] = display_name
    return display_names


def classify_result(result: BenchmarkResult) -> str:
    if result.outcome == OUTCOME_FAILURE:
        return BUCKET_FAILURE
    if result.outcome == OUTCOME_REGRESSION:
        return BUCKET_REGRESSION
    assert result.measurement is not None
    if result.measurement.ratio > 1.0 + NOISE_THRESHOLD:
        return BUCKET_SLOWER
    if result.measurement.ratio < 1.0 - NOISE_THRESHOLD:
        return BUCKET_FASTER
    return BUCKET_UNCHANGED


def format_run_count(result: BenchmarkResult) -> str:
    return str(result.initial_runs + result.confirmation_runs)


def benchmark_row(result: BenchmarkResult, display_name: str) -> BenchmarkRow:
    bucket = classify_result(result)
    if result.outcome == OUTCOME_FAILURE:
        return BenchmarkRow(
            display_name,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            "",
            format_run_count(result),
            bucket,
        )

    measurement = result.measurement
    assert measurement is not None
    delta, delta_percentage = delta_parts(measurement)
    return BenchmarkRow(
        display_name,
        format_duration_parts(measurement.old_timing),
        format_duration_parts(measurement.old_min),
        format_duration_parts(measurement.old_max),
        format_duration_parts(measurement.new_timing),
        format_duration_parts(measurement.new_min),
        format_duration_parts(measurement.new_max),
        delta,
        delta_percentage,
        format_run_count(result),
        bucket,
        ratio_change(measurement.ratio),
    )


def row_sort_key(row: BenchmarkRow):
    bucket_order = {
        BUCKET_UNCHANGED: 0,
        BUCKET_FASTER: 1,
        BUCKET_SLOWER: 2,
        BUCKET_REGRESSION: 3,
        BUCKET_FAILURE: 4,
    }
    if row.bucket == BUCKET_FASTER:
        bucket_value = -row.percentage
    elif row.bucket in (BUCKET_SLOWER, BUCKET_REGRESSION):
        bucket_value = row.percentage
    else:
        bucket_value = 0
    return bucket_order[row.bucket], bucket_value, row.display_name


def duration_slot(duration: FormattedDuration, number_width: int, unit_width: int) -> str:
    return f"{duration.value.rjust(number_width)} {duration.unit.ljust(unit_width)}"


def plain_timing_cell(
    timing: Optional[FormattedDuration],
    minimum: Optional[FormattedDuration],
    maximum: Optional[FormattedDuration],
    layout: TableLayout,
) -> str:
    if timing is None:
        return "failed"
    assert minimum is not None and maximum is not None
    timing_text = duration_slot(timing, layout.timing_number_width, layout.timing_unit_width)
    minimum_text = duration_slot(minimum, layout.timing_number_width, layout.timing_unit_width)
    maximum_text = duration_slot(maximum, layout.timing_number_width, layout.timing_unit_width)
    return f"{timing_text} [{minimum_text}…{maximum_text}]"


def styled_timing_cell(
    timing: Optional[FormattedDuration],
    minimum: Optional[FormattedDuration],
    maximum: Optional[FormattedDuration],
    layout: TableLayout,
) -> str:
    if timing is None:
        return "failed"
    assert minimum is not None and maximum is not None
    timing_text = duration_slot(timing, layout.timing_number_width, layout.timing_unit_width)
    minimum_text = duration_slot(minimum, layout.timing_number_width, layout.timing_unit_width)
    maximum_text = duration_slot(maximum, layout.timing_number_width, layout.timing_unit_width)
    return f"{timing_text} {gray(f'[{minimum_text}…{maximum_text}]')}"


def plain_delta_cell(row: BenchmarkRow, layout: TableLayout) -> str:
    if row.delta is None:
        return "failed"
    delta = duration_slot(row.delta, layout.delta_number_width, layout.delta_unit_width)
    return f"{delta} ({row.delta_percentage.rjust(layout.delta_percentage_width)})"


def table_layout(rows: List[BenchmarkRow]) -> TableLayout:
    durations = []
    deltas = []
    percentages = []
    for row in rows:
        for duration in (row.old_timing, row.old_min, row.old_max, row.new_timing, row.new_min, row.new_max):
            if duration is not None:
                durations.append(duration)
        if row.delta is not None:
            deltas.append(row.delta)
            percentages.append(row.delta_percentage)

    timing_number_width = max([len(duration.value) for duration in durations] + [1])
    timing_unit_width = max([len(duration.unit) for duration in durations] + [1])
    delta_number_width = max([len(duration.value) for duration in deltas] + [1])
    delta_unit_width = max([len(duration.unit) for duration in deltas] + [1])
    delta_percentage_width = max([len(percentage) for percentage in percentages] + [1])

    provisional = TableLayout(
        benchmark_width=max([len(row.display_name) for row in rows] + [len("benchmark")]),
        timing_number_width=timing_number_width,
        timing_unit_width=timing_unit_width,
        timing_width=0,
        delta_number_width=delta_number_width,
        delta_unit_width=delta_unit_width,
        delta_percentage_width=delta_percentage_width,
        delta_width=0,
        runs_width=max([len(row.runs) for row in rows] + [len("runs")]),
    )
    timing_width = max(
        [len(plain_timing_cell(row.old_timing, row.old_min, row.old_max, provisional)) for row in rows]
        + [len("base median"), len("PR median")]
    )
    delta_width = max([len(plain_delta_cell(row, provisional)) for row in rows] + [len("delta median")])
    provisional.timing_width = timing_width
    provisional.delta_width = delta_width
    return provisional


def render_table(rows: List[BenchmarkRow], layout: TableLayout):
    if not rows:
        return
    headers = ["benchmark", "base median", "PR median", "delta median", "runs"]
    plain_rows = [
        [
            row.display_name,
            plain_timing_cell(row.old_timing, row.old_min, row.old_max, layout),
            plain_timing_cell(row.new_timing, row.new_min, row.new_max, layout),
            plain_delta_cell(row, layout),
            row.runs,
        ]
        for row in rows
    ]
    widths = [
        layout.benchmark_width,
        layout.timing_width,
        layout.timing_width,
        layout.delta_width,
        layout.runs_width,
    ]

    print(
        "  ".join(
            headers[index].rjust(widths[index]) if index == 4 else headers[index].ljust(widths[index])
            for index in range(len(headers))
        )
    )
    print("  ".join("-" * widths[index] for index in range(len(headers))))
    for row, plain_row in zip(rows, plain_rows):
        styled_row = [
            row.display_name,
            styled_timing_cell(row.old_timing, row.old_min, row.old_max, layout),
            styled_timing_cell(row.new_timing, row.new_min, row.new_max, layout),
            color_change(row.bucket, plain_delta_cell(row, layout)),
            row.runs,
        ]
        cells = []
        for index, value in enumerate(styled_row):
            if index == 4:
                cells.append(" " * (widths[index] - len(plain_row[index])) + value)
            else:
                cells.append(value + " " * (widths[index] - len(plain_row[index])))
        print("  ".join(cells))


def print_bucket(title: str, rows: List[BenchmarkRow], layout: TableLayout, unchanged_count: int = 0):
    if title == "UNCHANGED (±3%)":
        if not unchanged_count:
            return
    elif not rows:
        return
    print("")
    if title == "UNCHANGED (±3%)":
        print(gray(title))
        print(gray(f"{unchanged_count} benchmarks"))
    else:
        print(title)
        render_table(rows, layout)


def geomean(values: List[float]) -> Optional[float]:
    if not values:
        return None
    return math.exp(math.fsum(math.log(value) for value in values) / len(values))


def geomean_regressed(old_geomean: Optional[float], new_geomean: Optional[float]) -> bool:
    if old_geomean is None or new_geomean is None:
        return False
    return new_geomean / old_geomean >= REGRESSION_LIMIT or new_geomean - old_geomean >= GEOMEAN_REGRESSION_SECONDS


def print_geomean_summary(old_geomean: Optional[float], new_geomean: Optional[float], sample_text: str):
    print("")
    if old_geomean is None or new_geomean is None:
        print(f"geomean: unavailable  {gray(sample_text)}")
        return
    old_text = format_duration(old_geomean)
    new_text = format_duration(new_geomean)
    measurement = BenchmarkMeasurement(
        old_timing=old_geomean,
        old_min=old_geomean,
        old_max=old_geomean,
        new_timing=new_geomean,
        new_min=new_geomean,
        new_max=new_geomean,
        ratio=new_geomean / old_geomean,
        runs=0,
    )
    if new_geomean > old_geomean:
        bucket = BUCKET_SLOWER
    elif new_geomean < old_geomean:
        bucket = BUCKET_FASTER
    else:
        bucket = BUCKET_UNCHANGED
    print(f"geomean: {old_text} -> {new_text}  {color_change(bucket, delta_text(measurement))}  {gray(sample_text)}")


def sampling_description(samples: Optional[int], rounded_samples: Optional[int], early_stop: bool) -> str:
    if samples is None:
        description = (
            f"sampling: adaptive; {INITIAL_RUNS} initial pairs, then "
            f"{MIN_CONFIRMATION_RUNS}–{MAX_CONFIRMATION_RUNS} confirmation pairs outside ±3%"
        )
    else:
        assert rounded_samples is not None
        description = f"sampling: fixed; {rounded_samples} samples per binary"
        if rounded_samples != samples:
            description += f" (--samples={samples} rounded up)"
        else:
            description += f" (--samples={samples})"
    if early_stop:
        description += "; median early stop"
    return description


def print_benchmark_report(
    rows: List[BenchmarkRow],
    missing_base_names: List[str],
    common_prefix: str,
    samples: Optional[int],
    rounded_samples: Optional[int],
    early_stop: bool,
):
    buckets = {
        bucket: [row for row in rows if row.bucket == bucket]
        for bucket in (BUCKET_UNCHANGED, BUCKET_FASTER, BUCKET_SLOWER, BUCKET_REGRESSION, BUCKET_FAILURE)
    }
    print("")
    benchmark_count = len(rows) + len(missing_base_names)
    suite_text = f"{common_prefix} ({benchmark_count} benchmarks)" if common_prefix else f"{benchmark_count} benchmarks"
    print(gray(f"suite: {suite_text}"))
    print(gray(sampling_description(samples, rounded_samples, early_stop)))
    print(gray("query regression: median change ≥ +10.0% (warning)"))
    print(gray("CI failure: geomean change ≥ +10.0% or ≥ +50.0 ms"))
    displayed_rows = [row for row in rows if row.bucket != BUCKET_UNCHANGED]
    layout = table_layout(displayed_rows)
    print_bucket("UNCHANGED (±3%)", buckets[BUCKET_UNCHANGED], layout, len(buckets[BUCKET_UNCHANGED]))
    print_bucket("FASTER", buckets[BUCKET_FASTER], layout)
    print_bucket("SLOWER (+3%…<+10%)", buckets[BUCKET_SLOWER], layout)
    print_bucket("REGRESSIONS (≥+10%)", buckets[BUCKET_REGRESSION], layout)
    print_bucket("FAILURES", buckets[BUCKET_FAILURE], layout)
    if missing_base_names:
        print("")
        print("SKIPPED (missing from Base)")
        for name in missing_base_names:
            print(f"{name}: PR smoke test passed")


def markdown_row(result: BenchmarkResult) -> str:
    measurement = result.measurement
    if measurement is None:
        return f"| `{result.benchmark}` | failed | failed | failed | `{format_run_count(result)}` |"
    old_timing, old_range = timing_parts(measurement.old_timing, measurement.old_min, measurement.old_max)
    new_timing, new_range = timing_parts(measurement.new_timing, measurement.new_min, measurement.new_max)
    return (
        f"| `{result.benchmark}` | `{old_timing} {old_range}` | `{new_timing} {new_range}` | "
        f"`{delta_text(measurement)}` | `{format_run_count(result)}` |"
    )


def report_query_regressions(results: List[BenchmarkResult], suite: str):
    if not results:
        return
    summary_lines = [
        f"## Query Regressions: `{suite}`",
        "",
        "| Benchmark | Base median | PR median | Delta median | Runs |",
        "| --- | --- | --- | --- | --- |",
    ]
    for result in results:
        measurement = result.measurement
        assert measurement is not None
        emit_github_warning(
            "Benchmark query regression",
            f"{suite}: {result.benchmark} median changed by {format_ratio_change(measurement.ratio)} "
            f"({format_duration(measurement.old_timing)} to {format_duration(measurement.new_timing)})",
        )
        summary_lines.append(markdown_row(result))
    append_step_summary(summary_lines + [""])


def report_failures(results: List[BenchmarkResult], suite: str):
    if not results:
        return
    summary_lines = [
        f"## Benchmark Failures: `{suite}`",
        "",
        "| Benchmark | Base median | PR median | Delta median | Runs |",
        "| --- | --- | --- | --- | --- |",
    ]
    for result in results:
        emit_github_error(
            "Regression benchmark failure",
            f"{suite}: {result.benchmark} failed while comparing base and PR benchmark runs",
        )
        summary_lines.append(markdown_row(result))
    append_step_summary(summary_lines + [""])


def report_missing_base(results: List[BenchmarkResult], suite: str):
    if not results:
        return
    summary_lines = [
        f"## Benchmarks Missing From Base: `{suite}`",
        "",
        "| Benchmark | PR smoke test |",
        "| --- | --- |",
    ]
    for result in results:
        emit_github_warning(
            "Benchmark missing from Linux Base",
            f"{suite}: {result.benchmark} does not exist in Linux Base; "
            "skipped comparison after a successful PR smoke test",
        )
        summary_lines.append(f"| `{result.benchmark}` | passed |")
    append_step_summary(summary_lines + [""])


def report_geomean_regression(old_geomean: float, new_geomean: float, nofail: bool):
    delta = new_geomean - old_geomean
    ratio = new_geomean / old_geomean
    message = (
        f"geomean changed from {format_duration(old_geomean)} to {format_duration(new_geomean)}; "
        f"{format_duration(delta, signed=True)} ({format_ratio_change(ratio)})"
    )
    if nofail:
        emit_github_warning("Geomean benchmark regression", message)
    else:
        emit_github_error("Geomean benchmark regression", message)
    append_step_summary(["## Geomean Regression", "", message, ""])


def print_failure_summary(failures: List[BenchmarkResult]):
    if not failures:
        return
    print(
        """\n
====================================================
================  FAILURES SUMMARY  ================
====================================================
"""
    )
    for index, result in enumerate(failures, start=1):
        print(f"{index}: {result.benchmark}")
        if result.old_failure != result.new_failure:
            print("Base:\n", result.old_failure or "No failure")
            print("PR:\n", result.new_failure or "No failure")
        else:
            print(result.old_failure)
        print("-", 52)


def query_regression_text(count: int) -> str:
    if count == 0:
        return "no query regressions"
    if count == 1:
        return "1 query regression"
    return f"{count} query regressions"


def print_result(
    execution_failed: bool,
    gate_failed: bool,
    nofail: bool,
    query_regressions: int,
    missing_base_count: int,
):
    query_text = query_regression_text(query_regressions)
    if missing_base_count == 1:
        missing_base_text = "; 1 benchmark skipped (missing from Base)"
    elif missing_base_count:
        missing_base_text = f"; {missing_base_count} benchmarks skipped (missing from Base)"
    else:
        missing_base_text = ""
    colored_query_text = (
        f"{ANSI_YELLOW}{query_text}{ANSI_RESET}" if query_regressions else f"{ANSI_GRAY}{query_text}{ANSI_RESET}"
    )
    if execution_failed:
        print(f"result: {ANSI_RED}failed; benchmark failure{ANSI_RESET}; {colored_query_text}{missing_base_text}")
    elif gate_failed and nofail:
        print(
            f"result: {ANSI_GREEN}passed (--nofail){ANSI_RESET}; "
            f"{ANSI_RED}geomean regression{ANSI_RESET}; {colored_query_text}{missing_base_text}"
        )
    elif gate_failed:
        print(f"result: {ANSI_RED}failed; geomean regression{ANSI_RESET}; {colored_query_text}{missing_base_text}")
    elif query_regressions:
        print(f"result: {ANSI_GREEN}passed{ANSI_RESET}; {colored_query_text}{missing_base_text}")
    else:
        print(f"result: {ANSI_GREEN}passed; no query regressions{ANSI_RESET}{missing_base_text}")


def main() -> int:
    args = parse_arguments()
    for label, path in (("base", args.old), ("PR", args.new)):
        if not os.path.isfile(path):
            print(f"Failed to find {label} runner {path}")
            return 1
    if not os.path.isfile(args.benchmarks):
        print(f"Failed to find benchmark list {args.benchmarks}")
        return 1

    old_tpcds_answers = Path(args.old_tpcds_answers).resolve() if args.old_tpcds_answers else None
    if old_tpcds_answers is not None and not old_tpcds_answers.is_dir():
        print(f"Failed to find base TPC-DS answers directory {old_tpcds_answers}")
        return 1

    isolated_directories = tempfile.TemporaryDirectory(prefix="duckdb-regression-benchmarks-")
    isolation_root = Path(isolated_directories.name)
    source_root = Path.cwd()
    old_root_directory = isolation_root / "base"
    new_root_directory = isolation_root / "pr"
    create_isolated_benchmark_root(source_root, old_root_directory, old_tpcds_answers)
    create_isolated_benchmark_root(source_root, new_root_directory)
    if args.verbose:
        print("benchmark cache: isolated Base and PR directories")

    with open(args.benchmarks, "r", encoding="utf-8") as benchmark_file:
        benchmarks = [line.strip() for line in benchmark_file if line.strip()]
    suite = Path(args.benchmarks).stem
    old_runner = BenchmarkRunner(
        args.old,
        "Base",
        args.threads,
        args.memory_limit,
        args.verbose,
        args.disable_timeout,
        args.benchmark_argument,
        root_directory=str(old_root_directory),
    )
    new_runner = BenchmarkRunner(
        args.new,
        "PR",
        args.threads,
        args.memory_limit,
        args.verbose,
        args.disable_timeout,
        args.benchmark_argument,
        root_directory=str(new_root_directory),
    )
    rounded_samples = sum(sampling_batch_sizes(args.samples)) if args.samples is not None else None
    if args.samples is None:
        results = [
            run_adaptive_benchmark(old_runner, new_runner, benchmark, args.early_stop, args.verbose)
            for benchmark in benchmarks
        ]
    else:
        results = [
            run_fixed_benchmark(old_runner, new_runner, benchmark, args.samples, args.early_stop, args.verbose)
            for benchmark in benchmarks
        ]

    execution_failures = [result for result in results if result.outcome == OUTCOME_FAILURE]
    query_regressions = [result for result in results if result.outcome == OUTCOME_REGRESSION]
    missing_base = [result for result in results if result.outcome == OUTCOME_MISSING_BASE]
    report_failures(execution_failures, suite)
    report_query_regressions(query_regressions, suite)
    report_missing_base(missing_base, suite)

    display_names = benchmark_display_names([result.benchmark for result in results])
    rows = [
        benchmark_row(result, display_names[result.benchmark])
        for result in results
        if result.outcome != OUTCOME_MISSING_BASE
    ]
    missing_base_names = [display_names[result.benchmark] for result in missing_base]
    rows.sort(key=row_sort_key)
    geomean_measurements = [
        result.initial_measurement if args.samples is None else result.measurement
        for result in results
        if (result.initial_measurement if args.samples is None else result.measurement) is not None
    ]
    old_geomean = geomean([measurement.old_timing for measurement in geomean_measurements])
    new_geomean = geomean([measurement.new_timing for measurement in geomean_measurements])
    gate_failed = geomean_regressed(old_geomean, new_geomean)
    if gate_failed:
        assert old_geomean is not None and new_geomean is not None
        report_geomean_regression(old_geomean, new_geomean, args.nofail)

    print_benchmark_report(
        rows,
        missing_base_names,
        benchmark_common_prefix([result.benchmark for result in results]),
        args.samples,
        rounded_samples,
        args.early_stop,
    )
    print_failure_summary(execution_failures)
    if args.samples is None:
        sample_text = f"(initial {INITIAL_RUNS} samples)"
    elif args.early_stop:
        sample_text = f"(up to {rounded_samples} samples)"
    else:
        sample_text = f"({rounded_samples} samples)"
    print_geomean_summary(old_geomean, new_geomean, sample_text)
    print_result(bool(execution_failures), gate_failed, args.nofail, len(query_regressions), len(missing_base))

    exit_code = 1 if execution_failures or (gate_failed and not args.nofail) else 0
    isolated_directories.cleanup()
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
