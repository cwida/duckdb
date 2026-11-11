import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET

import pytest


TRIVIAL = "statement ok\nSELECT 42;\n"
SUMMARY_HEADER = "================  FAILURES SUMMARY  ================"
CONNECTION_CONFIG = {
    "on_init": "SET VARIABLE initial_connection = true",
    "on_new_connection": (
        "SELECT CASE WHEN getvariable('initial_connection') THEN 1 ELSE error('connection failure probe') END"
    ),
}


def run_failure(unittest_binary, tmp_path, scripts, config=None, stdin=False, reporter="console"):
    config_path = tmp_path / "config.json"
    config_path.write_text(json.dumps({"summarize_failures": True, **(config or {})}))
    args = [
        unittest_binary,
        "--test-config",
        str(config_path),
        "--temp-dir-root",
        str(tmp_path / "test_dir"),
        "--use-colour",
        "no",
        "--reporter",
        reporter,
    ]
    if reporter == "junit":
        args.extend(["--out", str(tmp_path / "report.xml")])
    if stdin:
        names = ["<stdin>"]
        args.append("--stdin")
    else:
        names = []
        for index, script in enumerate(scripts):
            path = tmp_path / f"failure_{index}.test"
            path.write_text(script)
            names.append(path.as_posix())
        test_list = tmp_path / "tests.list"
        test_list.write_text("\n".join(names))
        args.extend(["--input-file", str(test_list)])
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_TEST_")}
    env.pop("NO_DUPLICATING_HEADERS", None)
    result = subprocess.run(
        args, input=scripts[0] if stdin else None, text=True, capture_output=True, env=env, timeout=30
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0, output
    assert SUMMARY_HEADER in result.stderr, output
    return result, names, result.stderr.split(SUMMARY_HEADER, 1)[1]


def assert_failure_location(output, name, line):
    # Catch uses parenthesized source locations with MSVC.
    assert re.search(rf"{re.escape(name)}(?::{line}|\({line}\)): FAILED:", output), output


@pytest.mark.parametrize("stdin", [False, True])
@pytest.mark.parametrize(
    "script,config,message,line",
    [
        pytest.param(
            TRIVIAL,
            {"on_init": "SELECT error('startup failure probe')"},
            "startup failure probe",
            0,
            id="startup",
        ),
        pytest.param(
            "load {TEST_DIR}/readonly.db\n\nload {TEST_DIR}/readonly.db readonly\n",
            {"on_init": "CREATE TABLE startup_probe(i INTEGER)"},
            "attached in read-only mode",
            0,
            id="readonly-startup",
        ),
        pytest.param(
            "statement ok other\nSELECT 42;\n",
            CONNECTION_CONFIG,
            "connection failure probe",
            0,
            id="new-connection",
        ),
        pytest.param(
            TRIVIAL,
            {"on_cleanup": "SELECT error('cleanup failure probe')"},
            "cleanup failure probe",
            0,
            id="cleanup",
        ),
        pytest.param(
            "set variable missing <variable:undefined_variable>\n",
            {},
            'Variable with name "undefined_variable" was not defined',
            0,
            id="exception",
        ),
        pytest.param("require-env\n", {}, "require-env requires", 1, id="parser"),
        pytest.param(
            "load {TEST_DIR}/missing.db readonly\n",
            {},
            "Failed to load database",
            0,
            id="database-load",
        ),
        pytest.param(
            "query I\nSELECT 42;\n----\n43\n",
            {},
            "Wrong result in query!",
            1,
            id="query-result",
        ),
    ],
)
def test_failure_reporting(unittest_binary, tmp_path, stdin, script, config, message, line):
    result, names, summary = run_failure(unittest_binary, tmp_path, [script], config, stdin)
    name = names[0]
    assert_failure_location(result.stdout, name, line)
    assert message in summary
    assert re.findall(r"^\d+\. (.+)$", summary, re.MULTILINE) == [f"{name}:{line}"]


def test_startup_failure_reports_each_test(unittest_binary, tmp_path):
    result, names, summary = run_failure(
        unittest_binary, tmp_path, [TRIVIAL, TRIVIAL], {"on_init": "SELECT error('startup failure probe')"}
    )
    for name in names:
        assert_failure_location(result.stdout, name, 0)
    assert sorted(re.findall(r"^\d+\. (.+)$", summary, re.MULTILINE)) == sorted(f"{name}:0" for name in names)
    assert summary.count("startup failure probe") == len(names)


def test_failure_locations_survive_until_junit_report(unittest_binary, tmp_path):
    result, names, _ = run_failure(
        unittest_binary,
        tmp_path,
        [TRIVIAL, TRIVIAL],
        {"on_init": "SELECT error('startup failure probe')"},
        reporter="junit",
    )
    failures = ET.parse(tmp_path / "report.xml").findall(".//failure")
    assert len(failures) == len(names)
    for name in names:
        assert any(f"{name}:0" in failure.text or f"{name}(0)" in failure.text for failure in failures)
