#!/usr/bin/env python3

import gzip
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from scripts import samply_profile

PROFILE_START_NS = 1_000_000_000


def empty_profile(pid: int = 42) -> dict:
    return {
        "meta": {
            "startTime": PROFILE_START_NS / 1_000_000,
            "categories": [],
            "markerSchema": [],
        },
        "threads": [
            {
                "pid": pid,
                "isMainThread": True,
                "processStartupTime": 0,
                "processShutdownTime": 1000,
                "stringArray": [],
                "markers": {
                    "category": [],
                    "data": [],
                    "endTime": [],
                    "name": [],
                    "phase": [],
                    "startTime": [],
                    "length": 0,
                },
            }
        ],
        "counters": [],
    }


def metric(
    query_id: int,
    name: str,
    value: str,
    start_ms: int | None = None,
    end_ms: int | None = None,
) -> dict:
    return {
        "query_id": query_id,
        "metric": name,
        "value": value,
        "start_ns": (None if start_ms is None else PROFILE_START_NS + start_ms * 1_000_000),
        "end_ns": None if end_ms is None else PROFILE_START_NS + end_ms * 1_000_000,
    }


class SamplyProfileTest(unittest.TestCase):
    def test_injects_query_and_phase_markers(self):
        profile = empty_profile()
        metrics = [
            metric(7, "query.sql", "SELECT 42"),
            metric(7, "query.total_time", "0.38", 120, 500),
            metric(7, "parser.total_time", "0.02", 100, 120),
            metric(7, "planner.total_time", "0.08", 120, 200),
            metric(7, "optimizer.total_time", "0.05", 200, 250),
            metric(7, "physical_planner.total_time", "0.05", 250, 300),
        ]
        query_logs = [
            {
                "query_id": 7,
                "timestamp_ns": PROFILE_START_NS + 300_000_000,
                "sql": "SELECT 42",
            }
        ]

        added = samply_profile.inject_query_markers(profile, 42, metrics, query_logs)

        self.assertEqual(added, 6)
        thread = profile["threads"][0]
        self.assertEqual(thread["markers"]["length"], 6)
        self.assertEqual(thread["markers"]["startTime"][0], 100)
        self.assertEqual(thread["markers"]["endTime"][0], 500)
        self.assertEqual(
            [data["name"] for data in thread["markers"]["data"]],
            [
                "Query: SELECT 42",
                "Parser",
                "Planner",
                "Optimizer",
                "Physical Planner",
                "Execution",
            ],
        )
        execution = thread["markers"]["data"][-1]
        self.assertEqual(execution["kind"], "phase")
        self.assertEqual(thread["markers"]["startTime"][-1], 300)
        self.assertEqual(thread["markers"]["endTime"][-1], 500)

    def test_ignores_metrics_without_query_bounds(self):
        profile = empty_profile()
        added = samply_profile.inject_query_markers(profile, 42, [metric(1, "query.sql", "SELECT 1")], [])
        self.assertEqual(added, 0)

    def test_injects_native_http_marker_pair(self):
        profile = empty_profile()
        events = [
            {
                "start_ns": PROFILE_START_NS + 200_000_000,
                "duration_ms": 25,
                "method": "GET",
                "url": "https://example.com/data",
                "status": 206,
                "request_range": "bytes=0-99",
                "response_range": "bytes 0-99/1000",
            }
        ]

        added = samply_profile.inject_http_markers(profile, 42, events)

        self.assertEqual(added, 1)
        markers = profile["threads"][0]["markers"]
        self.assertEqual(markers["length"], 2)
        self.assertEqual(markers["data"][0]["status"], "STATUS_START")
        self.assertEqual(markers["data"][1]["status"], "STATUS_STOP")
        self.assertEqual(markers["data"][1]["responseStatus"], 206)
        self.assertEqual(markers["data"][1]["responseContentRange"], "bytes 0-99/1000")

    def test_missing_response_creates_cancelled_http_marker(self):
        profile = empty_profile()
        event = {
            "start_ns": PROFILE_START_NS,
            "duration_ms": 0,
            "method": "HEAD",
            "url": "https://example.com/",
            "status": None,
            "request_range": "",
            "response_range": "",
        }
        samply_profile.inject_http_markers(profile, 42, [event])
        self.assertEqual(profile["threads"][0]["markers"]["data"][1]["status"], "STATUS_CANCEL")

    def test_requires_profiled_pid(self):
        event = {
            "start_ns": PROFILE_START_NS,
            "duration_ms": 0,
            "method": "HEAD",
            "url": "https://example.com/",
            "status": None,
            "request_range": "",
            "response_range": "",
        }
        with self.assertRaisesRegex(samply_profile.ProfileInjectionError, "process 99"):
            samply_profile.inject_http_markers(empty_profile(), 99, [event])

    def test_loads_plain_and_gzip_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            plain = Path(directory) / "plain.json"
            compressed = Path(directory) / "compressed.json.gz"
            plain.write_text(json.dumps(empty_profile()), encoding="utf-8")
            with gzip.open(compressed, "wt", encoding="utf-8") as output:
                json.dump(empty_profile(), output)
            self.assertEqual(samply_profile.load_profile(plain), empty_profile())
            self.assertEqual(samply_profile.load_profile(compressed), empty_profile())

    def test_writes_deterministic_gzip_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "profile.json.gz"
            samply_profile.write_profile_atomic(empty_profile(), output)
            first = output.read_bytes()
            samply_profile.write_profile_atomic(empty_profile(), output)
            self.assertEqual(output.read_bytes(), first)

    def test_build_launcher_argument_split(self):
        with tempfile.TemporaryDirectory() as directory:
            launcher = Path(directory) / "profile"
            (Path(directory) / "duckdb").touch()
            arguments = samply_profile.duckdb_arguments(
                ["--no-open", "--output", "out.json.gz", "--", "-f", "query.sql"],
                launcher,
            )
        self.assertEqual(
            arguments[-4:],
            [
                "--duckdb",
                str((launcher.parent / "duckdb").resolve()),
                "--",
                "-f",
                "query.sql",
            ][-4:],
        )
        self.assertEqual(arguments[:3], ["--no-open", "--output", "out.json.gz"])

    def test_default_build_launcher_arguments_are_duckdb_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            launcher = Path(directory) / "profile"
            duckdb = Path(directory) / "duckdb"
            duckdb.touch()
            arguments = samply_profile.duckdb_arguments(["-f", "query.sql"], launcher)
        self.assertEqual(arguments, ["--duckdb", str(duckdb.resolve()), "--", "-f", "query.sql"])

    def test_log_readers_convert_exported_rows(self):
        rows = [
            ["7", "query.total_time", "0.2", "100", "200"],
            ["7", "query.sql", "SELECT 1", "NULL", "NULL"],
        ]
        with mock.patch.object(samply_profile, "_query_csv", return_value=rows):
            events = samply_profile.read_metric_events(Path("duckdb"), Path("log"))
        self.assertEqual(events[0]["start_ns"], 100)
        self.assertIsNone(events[1]["start_ns"])

    def test_startup_sql_enables_only_required_collection(self):
        sql = samply_profile._startup_sql(Path("/tmp/a'b.tsv"))
        self.assertIn("['HTTP','Metrics','QueryLog']", sql)
        self.assertIn("query.total_time", sql)
        self.assertIn("physical_planner.total_time", sql)
        self.assertNotIn("memory", sql.lower())
        self.assertNotIn("network", sql.lower())
        self.assertIn("/tmp/a''b.tsv", sql)


if __name__ == "__main__":
    unittest.main()
