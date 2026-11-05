#!/usr/bin/env python3
"""Record the DuckDB shell with Samply and inject DuckDB timeline markers."""

from __future__ import annotations

import argparse
import csv
import gzip
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Iterable

PHASE_METRICS = (
    ("parser.total_time", "Parser"),
    ("planner.total_time", "Planner"),
    ("optimizer.total_time", "Optimizer"),
    ("physical_planner.total_time", "Physical Planner"),
)


class ProfileInjectionError(RuntimeError):
    pass


def _finite_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ProfileInjectionError(f"profile field {field} must be a finite number")
    return float(value)


def load_profile(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as raw_file:
            is_gzip = raw_file.read(2) == b"\x1f\x8b"
        opener = gzip.open if is_gzip else open
        with opener(path, "rt", encoding="utf-8") as profile_file:
            profile = json.load(profile_file)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProfileInjectionError(f"could not read Samply profile {path}: {error}") from error
    if not isinstance(profile, dict):
        raise ProfileInjectionError("Samply profile root must be an object")
    return profile


def write_profile_atomic(profile: dict[str, Any], output: Path) -> None:
    output = output.absolute()
    if not output.parent.is_dir():
        raise ProfileInjectionError(f"output directory does not exist: {output.parent}")
    temporary_path: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
        temporary_path = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as raw_file:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw_file, mtime=0) as gzip_file:
                gzip_file.write(json.dumps(profile, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
            raw_file.flush()
            os.fsync(raw_file.fileno())
        os.replace(temporary_path, output)
        temporary_path = None
    except OSError as error:
        raise ProfileInjectionError(f"could not atomically write {output}: {error}") from error
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def _sql_string(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def _log_relation(log_path: Path) -> str:
    columns = (
        "{'context_id':'UBIGINT','scope':'VARCHAR','connection_id':'UBIGINT',"
        "'transaction_id':'UBIGINT','query_id':'UBIGINT','thread_id':'UBIGINT',"
        "'timestamp':'TIMESTAMPTZ','type':'VARCHAR','log_level':'VARCHAR','message':'VARCHAR'}"
    )
    return f"read_csv({_sql_string(log_path)}, delim=',', header=true, " f"columns={columns}, quote='\"', escape='\"')"


def _query_csv(duckdb: Path, query: str) -> list[list[str]]:
    try:
        result = subprocess.run(
            [str(duckdb), "-no-init", "-csv", "-noheader", "-c", query],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
    except OSError as error:
        raise ProfileInjectionError(f"could not read DuckDB log with {duckdb}: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit status {result.returncode}"
        raise ProfileInjectionError(f"could not parse DuckDB profile log: {detail}")
    return list(csv.reader(io.StringIO(result.stdout)))


def read_metric_events(duckdb: Path, log_path: Path) -> list[dict[str, Any]]:
    query = f"""
        SELECT query_id, parsed.metric, parsed.value,
               epoch_ns(parsed.start_time), epoch_ns(parsed.end_time)
        FROM (
            SELECT query_id, timestamp, parse_duckdb_log_message('Metrics', message) AS parsed
            FROM {_log_relation(log_path)}
            WHERE type = 'Metrics' AND query_id IS NOT NULL
        )
        ORDER BY timestamp
    """
    events = []
    for row in _query_csv(duckdb, query):
        if len(row) != 5:
            raise ProfileInjectionError("invalid Metrics log export")
        events.append(
            {
                "query_id": int(row[0]),
                "metric": row[1],
                "value": row[2],
                "start_ns": _optional_int(row[3]),
                "end_ns": _optional_int(row[4]),
            }
        )
    return events


def read_query_events(duckdb: Path, log_path: Path) -> list[dict[str, Any]]:
    query = f"""
        SELECT query_id, epoch_ns(timestamp), message
        FROM {_log_relation(log_path)}
        WHERE type = 'QueryLog' AND query_id IS NOT NULL
        ORDER BY timestamp
    """
    events = []
    for row in _query_csv(duckdb, query):
        if len(row) != 3:
            raise ProfileInjectionError("invalid QueryLog export")
        events.append({"query_id": int(row[0]), "timestamp_ns": int(row[1]), "sql": row[2]})
    return events


def _status_code(value: str) -> int | None:
    match = re.search(r"(?:^|_)([1-5][0-9]{2})$", value)
    return int(match.group(1)) if match else None


def _optional_int(value: str) -> int | None:
    return None if not value or value == "NULL" else int(value)


def _optional_string(value: str) -> str:
    return "" if value == "NULL" else value


def read_http_events(duckdb: Path, log_path: Path) -> list[dict[str, Any]]:
    query = f"""
        SELECT epoch_ns(parsed.request.start_time), parsed.request.duration_ms,
               parsed.request.type, parsed.request.url, parsed.response.status,
               parsed.request.headers['Range'], parsed.response.headers['Content-Range']
        FROM (
            SELECT timestamp, parse_duckdb_log_message('HTTP', message) AS parsed
            FROM {_log_relation(log_path)}
            WHERE type = 'HTTP'
        )
        ORDER BY timestamp
    """
    events = []
    for row in _query_csv(duckdb, query):
        if len(row) != 7:
            raise ProfileInjectionError("invalid HTTP log export")
        start_ns = _optional_int(row[0])
        duration_ms = _optional_int(row[1])
        if start_ns is None or duration_ms is None:
            continue
        events.append(
            {
                "start_ns": start_ns,
                "duration_ms": max(0, duration_ms),
                "method": row[2],
                "url": row[3],
                "status": _status_code(row[4]),
                "request_range": _optional_string(row[5]),
                "response_range": _optional_string(row[6]),
            }
        )
    return events


def _matching_thread(profile: dict[str, Any], pid: int) -> tuple[int, dict[str, Any]]:
    threads = profile.get("threads")
    if not isinstance(threads, list) or not threads:
        raise ProfileInjectionError("profile threads must be a non-empty array")
    matches = []
    for index, thread in enumerate(threads):
        if not isinstance(thread, dict):
            continue
        try:
            matches_pid = int(float(thread.get("pid"))) == pid
        except (TypeError, ValueError):
            matches_pid = False
        if not matches_pid:
            continue
        try:
            startup_time = float(thread.get("processStartupTime"))
        except (TypeError, ValueError):
            startup_time = -math.inf
        matches.append((thread.get("isMainThread") is True, startup_time, index, thread))
    if not matches:
        raise ProfileInjectionError(f"Samply profile does not contain DuckDB process {pid}")
    _, _, index, thread = max(matches, key=lambda match: match[:3])
    return index, thread


def _profiled_process_pid(profile: dict[str, Any], process_name: str) -> int:
    matches = []
    for thread in profile.get("threads", []):
        if not isinstance(thread, dict) or thread.get("isMainThread") is not True:
            continue
        names = (thread.get("name"), thread.get("processName"))
        if not any(isinstance(name, str) and Path(name).name == process_name for name in names):
            continue
        try:
            pid = int(float(thread.get("pid")))
            startup_time = float(thread.get("processStartupTime"))
        except (TypeError, ValueError):
            continue
        matches.append((startup_time, pid))
    if not matches:
        raise ProfileInjectionError(f"Samply profile does not contain the {process_name} process")
    return max(matches)[1]


def _marker_table(thread: dict[str, Any], thread_index: int) -> dict[str, Any]:
    markers = thread.get("markers")
    if not isinstance(markers, dict):
        raise ProfileInjectionError(f"profile thread {thread_index} is missing its marker table")
    length = markers.get("length")
    if isinstance(length, bool) or not isinstance(length, int) or length < 0:
        raise ProfileInjectionError(f"profile thread {thread_index} has an invalid marker table length")
    for column in ("category", "data", "endTime", "name", "phase", "startTime"):
        values = markers.get(column)
        if not isinstance(values, list) or len(values) != length:
            raise ProfileInjectionError(f"profile thread {thread_index} has an invalid marker column {column}")
    string_array = thread.get("stringArray")
    if not isinstance(string_array, list) or not all(isinstance(value, str) for value in string_array):
        raise ProfileInjectionError(f"profile thread {thread_index} has an invalid string array")
    return markers


def _category(profile: dict[str, Any], name: str, color: str) -> int:
    meta = profile.get("meta")
    if not isinstance(meta, dict):
        raise ProfileInjectionError("profile is missing its meta object")
    categories = meta.get("categories")
    if not isinstance(categories, list):
        raise ProfileInjectionError("profile meta.categories must be an array")
    for index, category in enumerate(categories):
        if isinstance(category, dict) and category.get("name") == name:
            return index
    categories.append({"name": name, "color": color, "subcategories": ["Other"]})
    return len(categories) - 1


def _ensure_duckdb_marker_schema(profile: dict[str, Any]) -> None:
    meta = profile["meta"]
    schemas = meta.get("markerSchema")
    if schemas is None:
        schemas = []
        meta["markerSchema"] = schemas
    if not isinstance(schemas, list):
        raise ProfileInjectionError("profile meta.markerSchema must be an array")
    if any(isinstance(schema, dict) and schema.get("name") == "DuckDBProfile" for schema in schemas):
        return
    schemas.append(
        {
            "name": "DuckDBProfile",
            "display": ["marker-chart", "marker-table"],
            "chartLabel": "{marker.name}",
            "tooltipLabel": "{marker.name}",
            "tableLabel": "{marker.name}",
            "data": [
                {
                    "key": "name",
                    "label": "Name",
                    "format": "string",
                    "searchable": True,
                },
                {
                    "key": "kind",
                    "label": "Kind",
                    "format": "string",
                    "searchable": True,
                },
                {
                    "key": "metric",
                    "label": "Metric",
                    "format": "string",
                    "searchable": True,
                },
            ],
        }
    )


def _append_marker(
    thread: dict[str, Any],
    markers: dict[str, Any],
    *,
    category: int,
    title: str,
    data: dict[str, Any],
    start_time: float,
    end_time: float,
) -> None:
    name_index = len(thread["stringArray"])
    thread["stringArray"].append(title)
    markers["category"].append(category)
    markers["data"].append(data)
    markers["endTime"].append(end_time)
    markers["name"].append(name_index)
    markers["phase"].append(1)
    markers["startTime"].append(start_time)
    markers["length"] += 1


def _profile_time_bounds(
    profile: dict[str, Any], thread: dict[str, Any], thread_index: int
) -> tuple[float, float, float]:
    start_time = _finite_number(profile["meta"].get("startTime"), "meta.startTime")
    process_start = _finite_number(thread.get("processStartupTime"), f"threads[{thread_index}].processStartupTime")
    shutdown = thread.get("processShutdownTime")
    process_end = (
        math.inf if shutdown is None else _finite_number(shutdown, f"threads[{thread_index}].processShutdownTime")
    )
    return start_time, process_start, process_end


def _query_title(sql: str) -> str:
    normalized = " ".join(sql.split()).replace("\x00", "?") or "<empty>"
    prefix = "Query: "
    maximum_bytes = 900 - len(prefix.encode("utf-8"))
    if len(normalized.encode("utf-8")) <= maximum_bytes:
        return prefix + normalized
    truncated = normalized[:500]
    while len((truncated + "…").encode("utf-8")) > maximum_bytes:
        truncated = truncated[:-1]
    return prefix + truncated + "…"


def inject_query_markers(
    profile: dict[str, Any],
    pid: int,
    metric_events: Iterable[dict[str, Any]],
    query_events: Iterable[dict[str, Any]],
) -> int:
    metric_events = list(metric_events)
    if not metric_events:
        return 0
    thread_index, thread = _matching_thread(profile, pid)
    markers = _marker_table(thread, thread_index)
    profile_start, process_start, process_end = _profile_time_bounds(profile, thread, thread_index)
    category = _category(profile, "DuckDB", "purple")
    _ensure_duckdb_marker_schema(profile)

    metrics_by_query: dict[int, dict[str, dict[str, Any]]] = {}
    for event in metric_events:
        metrics_by_query.setdefault(event["query_id"], {})[event["metric"]] = event
    query_logs = {event["query_id"]: event for event in query_events}

    added = 0
    for query_id, metrics in metrics_by_query.items():
        total = metrics.get("query.total_time")
        if not total or total["start_ns"] is None or total["end_ns"] is None:
            continue
        timed_metrics = [
            event for event in metrics.values() if event["start_ns"] is not None and event["end_ns"] is not None
        ]
        query_start = min(event["start_ns"] for event in timed_metrics) / 1_000_000 - profile_start
        query_end = max(event["end_ns"] for event in timed_metrics) / 1_000_000 - profile_start
        if query_end < process_start or query_start > process_end:
            continue
        query_start = max(query_start, process_start)
        query_end = min(max(query_start, query_end), process_end)
        sql_metric = metrics.get("query.sql")
        sql = sql_metric["value"] if sql_metric else query_logs.get(query_id, {}).get("sql", "<unknown>")

        candidates = [
            (
                query_start,
                query_end,
                _query_title(sql),
                {"type": "DuckDBProfile", "name": _query_title(sql), "kind": "query"},
            )
        ]
        latest_phase_end = query_start
        for metric, title in PHASE_METRICS:
            event = metrics.get(metric)
            if not event or event["start_ns"] is None or event["end_ns"] is None:
                continue
            phase_start = max(query_start, event["start_ns"] / 1_000_000 - profile_start)
            phase_end = min(query_end, max(phase_start, event["end_ns"] / 1_000_000 - profile_start))
            latest_phase_end = max(latest_phase_end, phase_end)
            candidates.append(
                (
                    phase_start,
                    phase_end,
                    title,
                    {
                        "type": "DuckDBProfile",
                        "name": title,
                        "kind": "phase",
                        "metric": metric,
                    },
                )
            )

        query_log = query_logs.get(query_id)
        execution_start = latest_phase_end
        if query_log:
            execution_start = query_log["timestamp_ns"] / 1_000_000 - profile_start
        execution_start = min(query_end, max(query_start, execution_start))
        candidates.append(
            (
                execution_start,
                query_end,
                "Execution",
                {"type": "DuckDBProfile", "name": "Execution", "kind": "phase"},
            )
        )
        for start, end, title, data in sorted(candidates, key=lambda candidate: (candidate[0], -candidate[1])):
            _append_marker(
                thread,
                markers,
                category=category,
                title=title,
                data=data,
                start_time=start,
                end_time=end,
            )
            added += 1
    return added


def _next_network_marker_id(profile: dict[str, Any]) -> int:
    used_ids = set()
    for thread in profile.get("threads", []):
        for data in thread.get("markers", {}).get("data", []) if isinstance(thread, dict) else []:
            if isinstance(data, dict) and data.get("type") == "Network" and isinstance(data.get("id"), int):
                used_ids.add(data["id"])
    return max(used_ids, default=0) + 1


def inject_http_markers(profile: dict[str, Any], pid: int, http_events: Iterable[dict[str, Any]]) -> int:
    http_events = list(http_events)
    if not http_events:
        return 0
    thread_index, thread = _matching_thread(profile, pid)
    markers = _marker_table(thread, thread_index)
    profile_start, process_start, process_end = _profile_time_bounds(profile, thread, thread_index)
    category = _category(profile, "Network", "lightblue")
    marker_id = _next_network_marker_id(profile)
    added = 0

    for event in sorted(http_events, key=lambda item: item["start_ns"]):
        start = event["start_ns"] / 1_000_000 - profile_start
        end = start + event["duration_ms"]
        if end < process_start or start > process_end:
            continue
        start = max(start, process_start)
        end = min(max(start, end), process_end)
        title = f"{event['method']} {event['url']}"
        if event["request_range"]:
            title = f"{event['method']} [{event['request_range']}] {event['url']}"
        start_data = {
            "type": "Network",
            "URI": event["url"],
            "id": marker_id,
            "pri": 0,
            "status": "STATUS_START",
            "startTime": start,
            "endTime": start,
            "method": event["method"],
        }
        if event["request_range"]:
            start_data["requestRange"] = event["request_range"]
        stop_data = {
            **start_data,
            "status": "STATUS_STOP" if event["status"] is not None else "STATUS_CANCEL",
            "startTime": start,
            "endTime": end,
            "requestStart": start,
            "responseStart": end,
            "responseEnd": end,
        }
        if event["status"] is not None:
            stop_data["responseStatus"] = event["status"]
        if event["response_range"]:
            stop_data["responseContentRange"] = event["response_range"]
        _append_marker(
            thread,
            markers,
            category=category,
            title=title,
            data=start_data,
            start_time=start,
            end_time=start,
        )
        _append_marker(
            thread,
            markers,
            category=category,
            title=title,
            data=stop_data,
            start_time=start,
            end_time=end,
        )
        marker_id += 1
        added += 1
    return added


def inject_profile(raw_profile: Path, duckdb: Path, log_path: Path, output: Path) -> tuple[int, int]:
    profile = load_profile(raw_profile)
    pid = _profiled_process_pid(profile, duckdb.name)
    metric_events = read_metric_events(duckdb, log_path)
    query_events = read_query_events(duckdb, log_path)
    http_events = read_http_events(duckdb, log_path)
    query_markers = inject_query_markers(profile, pid, metric_events, query_events)
    http_markers = inject_http_markers(profile, pid, http_events)
    write_profile_atomic(profile, output)
    return query_markers, http_markers


def resolve_samply(argument: str | None) -> str:
    candidate = argument or os.environ.get("SAMPLY") or "samply"
    resolved = shutil.which(candidate)
    if resolved is None:
        raise ProfileInjectionError(f"could not find Samply executable: {candidate}")
    return resolved


def resolve_duckdb(launcher_path: str | Path) -> Path:
    duckdb = Path(launcher_path).resolve().with_name("duckdb")
    if not duckdb.is_file():
        raise ProfileInjectionError(f"could not find DuckDB executable next to profile launcher: {duckdb}")
    return duckdb


def duckdb_arguments(arguments: list[str], launcher_path: str | Path) -> list[str]:
    duckdb = resolve_duckdb(launcher_path)
    if "--" in arguments:
        separator = arguments.index("--")
        wrapper_arguments = arguments[:separator]
        command_arguments = arguments[separator + 1 :]
    else:
        wrapper_arguments = []
        command_arguments = arguments
    return [*wrapper_arguments, "--duckdb", str(duckdb), "--", *command_arguments]


def _startup_sql(log_path: Path) -> str:
    metrics = [
        "query.sql",
        "query.total_time",
        *(metric for metric, _ in PHASE_METRICS),
    ]
    metric_list = ", ".join(_sql_string(metric) for metric in metrics)
    return " ".join(
        (
            "CALL enable_logging(['HTTP','Metrics','QueryLog'], storage='file',",
            f"storage_config={{'path':{_sql_string(log_path)},'normalize':false}});",
            "SET profiling_coverage='ALL';",
            f"SET tracked_metrics=[{metric_list}];",
            "SET enable_profiling='no_output';",
        )
    )


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samply", help="path to Samply (then SAMPLY, then PATH)")
    parser.add_argument("--duckdb", required=True, help=argparse.SUPPRESS)
    parser.add_argument("--output", default="profile.json.gz", help="final gzip profile")
    parser.add_argument("--rate", type=float, help="Samply sampling rate in Hz")
    parser.add_argument("--duration", type=float, help="recording duration in seconds")
    parser.add_argument("--profile-name", help="custom Samply profile name")
    parser.add_argument("--no-open", action="store_true", help="do not open the final profile")
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="preserve the raw profile and DuckDB log",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER, help="DuckDB arguments, preceded by --")
    options = parser.parse_args(arguments)
    if options.command and options.command[0] == "--":
        options.command = options.command[1:]
    if options.rate is not None and (not math.isfinite(options.rate) or options.rate <= 0):
        parser.error("--rate must be positive")
    if options.duration is not None and (not math.isfinite(options.duration) or options.duration <= 0):
        parser.error("--duration must be positive")
    return options


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(arguments)
    duckdb = Path(options.duckdb).resolve()
    if not duckdb.is_file():
        print(
            f"samply_profile.py: could not find DuckDB executable: {duckdb}",
            file=sys.stderr,
        )
        return 1
    temporary_directory = Path(tempfile.mkdtemp(prefix="duckdb-samply-profile-"))
    raw_profile = temporary_directory / "raw-profile.json.gz"
    log_path = temporary_directory / "duckdb-log.csv"
    output = Path(options.output).absolute()
    preserve = options.keep_temp
    try:
        samply = resolve_samply(options.samply)
        target = [
            str(duckdb),
            "-cmd",
            _startup_sql(log_path),
            *options.command,
        ]
        record_command = [samply, "record", "--save-only", "--output", str(raw_profile)]
        if options.rate is not None:
            record_command.extend(["--rate", format(options.rate, "g")])
        if options.duration is not None:
            record_command.extend(["--duration", format(options.duration, "g")])
        if options.profile_name is not None:
            record_command.extend(["--profile-name", options.profile_name])
        record_result = subprocess.run([*record_command, "--", *target], check=False)
        if not raw_profile.is_file():
            preserve = True
            raise ProfileInjectionError(f"Samply did not create {raw_profile}")
        if not log_path.is_file():
            preserve = True
            raise ProfileInjectionError("DuckDB did not create its profiling log")
        query_markers, http_markers = inject_profile(raw_profile, duckdb, log_path, output)
        print(
            f"Injected {query_markers} DuckDB marker(s) and {http_markers} HTTP request(s) into {output}",
            file=sys.stderr,
        )

        load_status = 0
        if not options.no_open:
            load_status = subprocess.run([samply, "load", str(output)], check=False).returncode
        if record_result.returncode != 0:
            preserve = True
            return record_result.returncode
        if load_status != 0:
            preserve = True
            return load_status
        return 0
    except (OSError, ValueError, ProfileInjectionError) as error:
        preserve = True
        print(f"samply_profile.py: {error}", file=sys.stderr)
        return 1
    finally:
        if preserve:
            print(
                f"Raw profile and DuckDB log preserved in {temporary_directory}",
                file=sys.stderr,
            )
        else:
            shutil.rmtree(temporary_directory, ignore_errors=True)


def duckdb_main(arguments: list[str] | None = None, *, launcher_path: str | Path) -> int:
    try:
        return main(duckdb_arguments(list(arguments or []), launcher_path))
    except ProfileInjectionError as error:
        print(f"samply_profile.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
