import json
import os
import subprocess

import pytest


ENV_NAME = "DUCKDB_SQLLOGIC_REQUIRE_ENV_TEST"


def run_script(unittest_binary, tmp_path, script, value=None, source="environment", extra_args=()):
    env = os.environ.copy()
    env.pop(ENV_NAME, None)
    args = [unittest_binary, "--stdin", "--emit-test-events", *extra_args]
    if source == "config":
        config = tmp_path / "config.json"
        config.write_text(
            json.dumps({"test_env": [] if value is None else [{"env_name": ENV_NAME, "env_value": value}]})
        )
        args.extend(["--test-config", str(config)])
    elif value is not None:
        env[ENV_NAME] = value
        if source == "passthrough":
            args.extend(["--env-passthrough", ENV_NAME])
    result = subprocess.run(args, input=script, text=True, capture_output=True, env=env, timeout=30)
    events = [
        json.loads(line.split("[TEST_EVENT] ", 1)[1]) for line in result.stderr.splitlines() if "[TEST_EVENT] " in line
    ]
    ends = [event for event in events if event["event"] == "end"]
    assert len(ends) == 1, result.stdout + result.stderr
    return result, ends[0]


@pytest.mark.parametrize("source", ["environment", "config", "passthrough"])
@pytest.mark.parametrize(
    "directive,values,value,runs",
    [
        ("require-env", "", "fixture", True),
        ("require-env", "", "", True),
        ("require-env", "fixture", "fixture", True),
        ("require-env", "fixture", "nessie", False),
        ("require-env", "fixture lakekeeper", "fixture", True),
        ("require-env", "fixture lakekeeper", "lakekeeper", True),
        ("require-env", "fixture lakekeeper", "nessie", False),
        ("require-env", "fixture lakekeeper", "Fixture", False),
        ("require-env", "fixture", "", False),
        ("require-env", "!nessie", "!nessie", True),
        ("require-env", "not", "not", True),
        ("require-env", "fixture,nessie", "fixture,nessie", True),
        ("require-env-not", "", "fixture", False),
        ("require-env-not", "", "", False),
        ("require-env-not", "nessie", "fixture", True),
        ("require-env-not", "nessie", "nessie", False),
        ("require-env-not", "nessie polaris", "fixture", True),
        ("require-env-not", "nessie polaris", "nessie", False),
        ("require-env-not", "nessie polaris", "polaris", False),
        ("require-env-not", "nessie polaris", "Nessie", True),
        ("require-env-not", "nessie polaris", "", True),
    ],
)
def test_require_env_values(unittest_binary, tmp_path, source, directive, values, value, runs):
    header = f"{directive} {ENV_NAME}" + (f" {values}" if values else "")
    body = f"query I\nSELECT '{{{ENV_NAME}}}' = '{value}'\n----\ntrue\n"
    result, end = run_script(unittest_binary, tmp_path, f"{header}\n\n{body}", value, source)
    assert result.returncode == 0, result.stdout + result.stderr
    assert end["status"] == ("ok" if runs else "skip-requirement")
    assert end["passes"] == (1 if runs else 0)
    if not runs:
        assert end["data"] == header


@pytest.mark.parametrize("source", ["environment", "config"])
@pytest.mark.parametrize(
    "directive,values",
    [("require-env", ""), ("require-env", "fixture lakekeeper"), ("require-env-not", "nessie polaris")],
)
def test_require_env_unset(unittest_binary, tmp_path, source, directive, values):
    header = f"{directive} {ENV_NAME}" + (f" {values}" if values else "")
    result, end = run_script(
        unittest_binary, tmp_path, f"{header}\n\nstatement ok\nSELECT missing_column;\n", source=source
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert end["status"] == "skip-requirement"
    assert end["passes"] == 0
    assert end["data"] == header


@pytest.mark.parametrize(
    "header,error",
    [
        ("require-env", "require-env requires"),
        ("require-env-not", "require-env-not requires"),
        (f"loop i 0 1\n\nrequire-env-not {ENV_NAME}", "require-env-not cannot be called in a loop"),
        (f"loop i 0 1\n\nrequire-env {ENV_NAME} fixture", "require-env cannot be called in a loop"),
        (f"loop i 0 1\n\nrequire-env-not {ENV_NAME} nessie", "require-env-not cannot be called in a loop"),
    ],
)
def test_require_env_invalid(unittest_binary, tmp_path, header, error):
    result, end = run_script(unittest_binary, tmp_path, f"{header}\n\n", "fixture")
    assert result.returncode != 0
    assert error in result.stdout + result.stderr


@pytest.mark.parametrize("source", ["environment", "config"])
def test_require_env_not_absent(unittest_binary, tmp_path, source):
    # Repeated absence checks must neither define the variable nor add a presence tag.
    header = f"require-env-not {ENV_NAME}\n\n"
    script = header * 2 + "query I\nSELECT 42\n----\n42\n"
    result, end = run_script(
        unittest_binary, tmp_path, script, source=source, extra_args=["--skip-tag", f"env[{ENV_NAME}]"]
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert end["status"] == "ok"
    assert end["passes"] == 1


@pytest.mark.parametrize("directive,values", [("require-env", "nessie fixture"), ("require-env-not", "nessie polaris")])
@pytest.mark.parametrize("tag_value,runs", [("fixture", False), ("nessie", True)])
def test_require_env_actual_value_tag(unittest_binary, tmp_path, directive, values, tag_value, runs):
    script = f"{directive} {ENV_NAME} {values}\n\nquery I\nSELECT 42\n----\n42\n"
    result, end = run_script(
        unittest_binary,
        tmp_path,
        script,
        "fixture",
        extra_args=["--skip-tag", f"env[{ENV_NAME}]={tag_value}"],
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert end["status"] == ("ok" if runs else "skip-requirement")
    assert end["passes"] == (1 if runs else 0)
    if not runs:
        assert end["data"] == "select tag-set"
