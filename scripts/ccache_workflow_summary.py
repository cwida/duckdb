#!/usr/bin/env python3
"""
Summarize ccache usage for a GitHub Actions workflow run.

Examples:
  python3 scripts/ccache_workflow_summary.py 22331644275
  python3 scripts/ccache_workflow_summary.py 22331644275 --all-jobs
  python3 scripts/ccache_workflow_summary.py 22331644275 --repo duckdb/duckdb --format csv --output summary.csv
  python3 scripts/ccache_workflow_summary.py 22331644275 --attempt 2 --threshold 90 --verbose
"""

import argparse
import csv
import io
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from typing import Any, Dict, List, Optional, Sequence


MAX_LOG_WORKERS = 8
CCACHE_STEP_RE = re.compile(r"ccache", re.IGNORECASE)
CCACHE_VERSION_RE = re.compile(r"\bccache version\s+(\S+)", re.IGNORECASE)
CACHEABLE_CALLS_RE = re.compile(r"Cacheable calls:\s*(\d+)\s*/\s*(\d+)", re.IGNORECASE)
HITS_RE = re.compile(r"\bHits:\s*(\d+)\s*/\s*(\d+)", re.IGNORECASE)
MISSES_RE = re.compile(r"\bMisses:\s*(\d+)\s*/\s*(\d+)", re.IGNORECASE)
CCACHE_RESTORE_HIT_RE = re.compile(
    r"(?:Cache hit for restore-key|Cache restored from key):\s*[\"']?ccache-", re.IGNORECASE
)
RESTORE_GROUP_RE = re.compile(r"(?:##\[group\]|::group::)Restore cache", re.IGNORECASE)
NO_CACHE_RE = re.compile(r"No cache found\.", re.IGNORECASE)


@dataclass(frozen=True)
class CCacheStats:
    hits: int
    misses: int
    cacheable_calls: int
    ccache_version: Optional[str]

    @property
    def hit_rate_percent(self) -> float:
        if self.cacheable_calls == 0:
            return 0.0
        return 100.0 * self.hits / self.cacheable_calls


@dataclass(frozen=True)
class JobResult:
    job_id: int
    job_name: str
    conclusion: str
    ccache_configured: bool
    cache_restore_status: str
    stats_available: bool
    hits: Optional[int]
    misses: Optional[int]
    cacheable_calls: Optional[int]
    hit_rate_percent: Optional[float]
    ccache_version: Optional[str]
    log_status: str
    log_error: Optional[str]


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def percentage(value: str) -> float:
    parsed = float(value)
    if parsed < 0 or parsed > 100:
        raise argparse.ArgumentTypeError("must be between 0 and 100")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze ccache usage for a GitHub Actions run via gh CLI.")
    parser.add_argument("run_id", type=positive_int, help="GitHub Actions run ID (for example: 22331644275).")
    parser.add_argument("--repo", default="duckdb/duckdb", help="Repository in owner/name format.")
    parser.add_argument("--attempt", type=positive_int, help="Workflow attempt number; defaults to the latest attempt.")
    parser.add_argument(
        "--format",
        choices=["markdown", "table", "csv", "json"],
        default="markdown",
        help="Output format (default: markdown).",
    )
    parser.add_argument("--output", default="", help="Optional output file path; otherwise prints to stdout.")
    parser.add_argument(
        "--threshold",
        type=percentage,
        default=80.0,
        help="Hit rate below which a job is highlighted (default: 80).",
    )
    parser.add_argument(
        "--all-jobs",
        action="store_true",
        help="Include the complete per-job table in human-readable output.",
    )
    parser.add_argument("--verbose", action="store_true", help="Print progress messages to stderr.")
    return parser.parse_args()


def run_gh(args: Sequence[str]) -> str:
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        cmd = " ".join(args)
        raise RuntimeError(f"Command failed ({proc.returncode}): {cmd}\n{proc.stderr.strip()}")
    return proc.stdout


def get_jobs(repo: str, run_id: int, attempt: Optional[int]) -> tuple[List[Dict[str, Any]], int]:
    command = ["gh", "run", "view", str(run_id), "--repo", repo]
    if attempt is not None:
        command.extend(["--attempt", str(attempt)])
    command.extend(["--json", "attempt,jobs"])
    payload = json.loads(run_gh(command))
    if not isinstance(payload, dict):
        raise RuntimeError("The GitHub CLI returned an invalid workflow run payload.")
    jobs = payload.get("jobs", [])
    if not isinstance(jobs, list):
        raise RuntimeError("The GitHub CLI returned an invalid jobs payload.")
    actual_attempt = int(payload.get("attempt") or attempt or 1)
    return jobs, actual_attempt


def job_has_ccache_step(job: Dict[str, Any]) -> bool:
    for step in job.get("steps", []):
        if step.get("conclusion") == "skipped":
            continue
        if CCACHE_STEP_RE.search(str(step.get("name") or "")):
            return True
    return False


def parse_ccache_stats(log_text: str) -> Optional[CCacheStats]:
    lines = log_text.splitlines()
    versions = [match.group(1) for line in lines if (match := CCACHE_VERSION_RE.search(line))]
    version = versions[-1] if versions else None
    candidates: List[CCacheStats] = []

    for index, line in enumerate(lines):
        cacheable_match = CACHEABLE_CALLS_RE.search(line)
        if cacheable_match is None:
            continue

        cacheable_calls = int(cacheable_match.group(1))
        hits: Optional[int] = None
        misses: Optional[int] = None
        for stats_line in lines[index + 1 : index + 16]:
            if CACHEABLE_CALLS_RE.search(stats_line):
                break
            if hits is None:
                hits_match = HITS_RE.search(stats_line)
                if hits_match is not None:
                    hits = int(hits_match.group(1))
            if misses is None:
                misses_match = MISSES_RE.search(stats_line)
                if misses_match is not None:
                    misses = int(misses_match.group(1))
            if hits is not None and misses is not None:
                break

        if hits is None or hits > cacheable_calls:
            continue
        if misses is None:
            misses = cacheable_calls - hits
        if hits + misses != cacheable_calls:
            continue
        candidates.append(CCacheStats(hits, misses, cacheable_calls, version))

    return candidates[-1] if candidates else None


def parse_cache_restore_status(log_text: str, ccache_configured: bool) -> str:
    if not ccache_configured:
        return "not_configured"
    if CCACHE_RESTORE_HIT_RE.search(log_text):
        return "hit"

    lines = log_text.splitlines()
    for index, line in enumerate(lines):
        if not NO_CACHE_RE.search(line):
            continue
        window = "\n".join(lines[max(0, index - 30) : index + 1])
        if RESTORE_GROUP_RE.search(window) and ("ccache-action" in window.lower() or "ccache-" in window.lower()):
            return "miss"
    return "unknown"


def fetch_job_result(repo: str, run_id: int, job: Dict[str, Any]) -> JobResult:
    job_id = int(job.get("databaseId") or 0)
    job_name = str(job.get("name") or "")
    conclusion = str(job.get("conclusion") or "")
    configured_from_steps = job_has_ccache_step(job)

    try:
        log_text = run_gh(["gh", "run", "view", str(run_id), "--repo", repo, "--job", str(job_id), "--log"])
    except (OSError, RuntimeError) as error:
        return JobResult(
            job_id=job_id,
            job_name=job_name,
            conclusion=conclusion,
            ccache_configured=configured_from_steps,
            cache_restore_status="unknown" if configured_from_steps else "not_configured",
            stats_available=False,
            hits=None,
            misses=None,
            cacheable_calls=None,
            hit_rate_percent=None,
            ccache_version=None,
            log_status="error",
            log_error=str(error),
        )

    stats = parse_ccache_stats(log_text)
    ccache_configured = configured_from_steps or stats is not None
    restore_status = parse_cache_restore_status(log_text, ccache_configured)
    return JobResult(
        job_id=job_id,
        job_name=job_name,
        conclusion=conclusion,
        ccache_configured=ccache_configured,
        cache_restore_status=restore_status,
        stats_available=stats is not None,
        hits=stats.hits if stats else None,
        misses=stats.misses if stats else None,
        cacheable_calls=stats.cacheable_calls if stats else None,
        hit_rate_percent=stats.hit_rate_percent if stats else None,
        ccache_version=stats.ccache_version if stats else None,
        log_status="ok",
        log_error=None,
    )


def fetch_job_results(repo: str, run_id: int, jobs: List[Dict[str, Any]], verbose: bool = False) -> List[JobResult]:
    if not jobs:
        return []

    results: List[Optional[JobResult]] = [None] * len(jobs)
    with ThreadPoolExecutor(max_workers=min(MAX_LOG_WORKERS, len(jobs))) as executor:
        futures = {executor.submit(fetch_job_result, repo, run_id, job): index for index, job in enumerate(jobs)}
        for future in as_completed(futures):
            index = futures[future]
            result = future.result()
            results[index] = result
            if verbose:
                print(f"Processed job {result.job_id}: {result.job_name}", file=sys.stderr, flush=True)

    return [result for result in results if result is not None]


def summarize_results(results: List[JobResult]) -> Dict[str, Any]:
    configured = [result for result in results if result.ccache_configured]
    with_stats = [result for result in configured if result.stats_available]
    total_hits = sum(result.hits or 0 for result in with_stats)
    total_misses = sum(result.misses or 0 for result in with_stats)
    total_cacheable_calls = sum(result.cacheable_calls or 0 for result in with_stats)
    weighted_rate = 100.0 * total_hits / total_cacheable_calls if total_cacheable_calls else None
    mean_rate = sum(result.hit_rate_percent or 0.0 for result in with_stats) / len(with_stats) if with_stats else None

    return {
        "total_jobs": len(results),
        "ccache_configured_jobs": len(configured),
        "stats_available_jobs": len(with_stats),
        "cache_restore_hits": sum(result.cache_restore_status == "hit" for result in configured),
        "cache_restore_misses": sum(result.cache_restore_status == "miss" for result in configured),
        "cache_restore_unknown": sum(result.cache_restore_status == "unknown" for result in configured),
        "log_error_jobs": sum(result.log_status == "error" for result in results),
        "total_hits": total_hits,
        "total_misses": total_misses,
        "total_cacheable_calls": total_cacheable_calls,
        "weighted_hit_rate_percent": weighted_rate,
        "mean_job_hit_rate_percent": mean_rate,
    }


def format_percent(value: Optional[float]) -> str:
    return "N/A" if value is None else f"{value:.2f}%"


def result_row(result: JobResult) -> Dict[str, str]:
    return {
        "job_name": result.job_name,
        "conclusion": result.conclusion,
        "ccache": "Yes" if result.ccache_configured else "No",
        "restore": result.cache_restore_status,
        "hits": "N/A" if result.hits is None else str(result.hits),
        "cacheable_calls": "N/A" if result.cacheable_calls is None else str(result.cacheable_calls),
        "hit_rate": format_percent(result.hit_rate_percent),
        "log": result.log_status,
    }


def escape_markdown(cell: str) -> str:
    return cell.replace("\\", "\\\\").replace("|", "\\|").replace("\n", "<br>")


def format_markdown_table(rows: List[Dict[str, str]], columns: List[str]) -> str:
    header = "| " + " | ".join(columns) + " |"
    separator = "| " + " | ".join(["---"] * len(columns)) + " |"
    body = ["| " + " | ".join(escape_markdown(str(row.get(column, ""))) for column in columns) + " |" for row in rows]
    return "\n".join([header, separator] + body)


def format_table(rows: List[Dict[str, str]], columns: List[str]) -> str:
    widths = {column: len(column) for column in columns}
    for row in rows:
        for column in columns:
            widths[column] = max(widths[column], len(str(row.get(column, ""))))
    header = "  ".join(column.ljust(widths[column]) for column in columns)
    separator = "  ".join("-" * widths[column] for column in columns)
    body = ["  ".join(str(row.get(column, "")).ljust(widths[column]) for column in columns) for row in rows]
    return "\n".join([header, separator] + body)


def human_summary(summary: Dict[str, Any]) -> List[str]:
    return [
        (
            f"Jobs: {summary['total_jobs']} total; {summary['ccache_configured_jobs']} configured for ccache; "
            f"{summary['stats_available_jobs']} with statistics."
        ),
        (
            f"Restore: {summary['cache_restore_hits']} hits; {summary['cache_restore_misses']} misses; "
            f"{summary['cache_restore_unknown']} unknown."
        ),
        (
            f"Hit rate: {format_percent(summary['weighted_hit_rate_percent'])} weighted "
            f"({summary['total_hits']} / {summary['total_cacheable_calls']}); "
            f"{format_percent(summary['mean_job_hit_rate_percent'])} mean across jobs."
        ),
        f"Log errors: {summary['log_error_jobs']}.",
    ]


def format_human_output(
    results: List[JobResult], summary: Dict[str, Any], output_format: str, threshold: float, all_jobs: bool
) -> str:
    markdown = output_format == "markdown"
    parts: List[str] = []
    summary_lines = human_summary(summary)
    if markdown:
        parts.append("# Ccache workflow summary\n\n" + "\n".join(f"- {line}" for line in summary_lines))
    else:
        parts.append("CCache workflow summary\n" + "\n".join(summary_lines))

    low_hit_results = sorted(
        (result for result in results if result.hit_rate_percent is not None and result.hit_rate_percent < threshold),
        key=lambda result: (result.hit_rate_percent, result.job_name),
    )
    incomplete_results = [
        result
        for result in results
        if result.ccache_configured and (not result.stats_available or result.log_status == "error")
    ]
    columns = ["job_name", "conclusion", "restore", "hits", "cacheable_calls", "hit_rate"]
    formatter = format_markdown_table if markdown else format_table

    low_title = f"Jobs below {threshold:g}%"
    low_body = formatter([result_row(result) for result in low_hit_results], columns) if low_hit_results else "None."
    parts.append((f"## {low_title}" if markdown else low_title) + "\n\n" + low_body)

    incomplete_columns = ["job_name", "conclusion", "restore", "log"]
    incomplete_body = (
        formatter([result_row(result) for result in incomplete_results], incomplete_columns)
        if incomplete_results
        else "None."
    )
    parts.append(("## Missing statistics" if markdown else "Missing statistics") + "\n\n" + incomplete_body)

    if all_jobs:
        all_columns = ["job_name", "conclusion", "ccache", "restore", "hits", "cacheable_calls", "hit_rate", "log"]
        all_body = formatter([result_row(result) for result in results], all_columns)
        parts.append(("## All jobs" if markdown else "All jobs") + "\n\n" + all_body)
    return "\n\n".join(parts)


def job_to_machine_row(result: JobResult) -> Dict[str, Any]:
    return asdict(result)


def to_csv(results: List[JobResult]) -> str:
    columns = [
        "job_id",
        "job_name",
        "conclusion",
        "ccache_configured",
        "cache_restore_status",
        "stats_available",
        "hits",
        "misses",
        "cacheable_calls",
        "hit_rate_percent",
        "ccache_version",
        "log_status",
        "log_error",
    ]
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=columns, lineterminator="\n")
    writer.writeheader()
    for result in results:
        row = job_to_machine_row(result)
        row["ccache_configured"] = str(result.ccache_configured).lower()
        row["stats_available"] = str(result.stats_available).lower()
        writer.writerow(row)
    return output.getvalue().rstrip("\n")


def format_output(
    repo: str,
    run_id: int,
    attempt: Optional[int],
    results: List[JobResult],
    output_format: str,
    threshold: float,
    all_jobs: bool,
) -> str:
    summary = summarize_results(results)
    if output_format == "json":
        payload = {
            "repository": repo,
            "run_id": run_id,
            "attempt": attempt,
            "threshold_percent": threshold,
            "summary": summary,
            "jobs": [job_to_machine_row(result) for result in results],
        }
        return json.dumps(payload, indent=2)
    if output_format == "csv":
        return to_csv(results)
    return format_human_output(results, summary, output_format, threshold, all_jobs)


def emit(text: str, output_path: str) -> None:
    if output_path:
        with open(output_path, "w", encoding="utf-8") as output_file:
            output_file.write(text)
            if not text.endswith("\n"):
                output_file.write("\n")
        return
    print(text)


def main() -> int:
    args = parse_args()
    try:
        print(
            f"Fetching workflow run logs for run {args.run_id} from {args.repo}...",
            file=sys.stderr,
            flush=True,
        )
        all_jobs, actual_attempt = get_jobs(args.repo, args.run_id, args.attempt)
        jobs = [job for job in all_jobs if job.get("conclusion") != "skipped"]
        if args.verbose:
            print(f"Fetching {len(jobs)} non-skipped job logs with up to {MAX_LOG_WORKERS} workers.", file=sys.stderr)
        results = fetch_job_results(args.repo, args.run_id, jobs, args.verbose)
        text = format_output(
            args.repo,
            args.run_id,
            actual_attempt,
            results,
            args.format,
            args.threshold,
            args.all_jobs,
        )
        emit(text, args.output)
        return 2 if any(result.log_status == "error" for result in results) else 0
    except (json.JSONDecodeError, OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
