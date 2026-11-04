"""Contract checks for the unittest binary's --temp-dir-* family.

These assert lifecycle behaviour that no .test file can observe: what exists on disk *after* a
run, and what a run is allowed to create on an error path. Every case here corresponds to a
regression the full sqllogictest suite passed straight through.

Point at a binary with DUCKDB_UNITTEST_BINARY, or let discovery find
build/{debug,reldebug,release}/test/unittest under DUCKDB_ROOT (default: cwd) or its parent --
the parent covers an out-of-tree extension building this repo as a submodule.
"""

from __future__ import annotations

import csv
import json
import os
import re
import shlex
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

# A trivial always-passing body; the temp dir is materialized either way.
TRIVIAL = """# name: probe.test
# group: [compat]

query I
SELECT 42
----
42
"""


# Dumps the substituted contract vars so the harness can assert on them from outside.
PROBE = """# name: probe.test
# group: [compat]

statement ok
COPY (SELECT '{{TEMP_DIR}}' AS temp_dir, '{{TEMP_DIR_ABSOLUTE}}' AS temp_dir_absolute,
             '{{TEMP_DIR_ROOT}}' AS temp_dir_root, '{{LOCAL_TEMP_DIR}}' AS local_temp_dir,
             '{{DATA_DIR}}' AS data_dir, '{{LOCAL_DATA_DIR}}' AS local_data_dir,
             '{{RUN_ID}}' AS run_id, '{{TEST_ID}}' AS test_id)
     TO '{out}' (FORMAT csv, HEADER)
"""


def _discover_binary():
    explicit = os.environ.get("DUCKDB_UNITTEST_BINARY")
    if explicit:
        return Path(explicit)
    root = Path(os.environ.get("DUCKDB_ROOT", os.getcwd()))
    for base in (root, root.parent):
        for flavour in ("debug", "reldebug", "release"):
            candidate = base / "build" / flavour / "test" / "unittest"
            if candidate.is_file():
                return candidate
    return None


@pytest.fixture(scope="session")
def unittest_bin() -> Path:
    binary = _discover_binary()
    if binary is None or not binary.is_file():
        pytest.skip("no unittest binary; set DUCKDB_UNITTEST_BINARY")
    # Confirm it actually loads here. A binary built against a newer libc/libstdc++ dies before main,
    # which would otherwise surface as every assertion failing for its own inscrutable reason.
    probe = subprocess.run([str(binary), "--help"], capture_output=True, text=True, timeout=60)
    if "not found" in probe.stderr and "version `" in probe.stderr:
        pytest.fail(f"{binary} cannot run here:\n{probe.stderr.strip().splitlines()[0]}")
    return binary


@pytest.fixture(scope="session")
def duckdb_root() -> Path:
    return Path(os.environ.get("DUCKDB_ROOT", os.getcwd()))


@pytest.fixture
def run(unittest_bin: Path, duckdb_root: Path):
    """Run the binary against a .test body fed on stdin. Returns the CompletedProcess."""

    def _run(body: str = TRIVIAL, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        # Every case here asserts on an exact configuration, and every option also has a
        # DUCKDB_TEST_<NAME> form -- so an ambient one silently rewrites what is under test. Drop the
        # whole prefix, then let the case put back only what it means to set.
        ambient = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_TEST_")}
        return subprocess.run(
            [str(unittest_bin), "--test-dir", str(duckdb_root), "--stdin", *args],
            input=body,
            capture_output=True,
            text=True,
            env={**ambient, **(env or {})},
            timeout=300,
        )

    return _run


@pytest.fixture
def probe(run, tmp_path: Path):
    """Run a body that dumps the contract vars; return them as a dict."""

    def _probe(*args: str, env: dict | None = None) -> dict:
        out = tmp_path / "vars.csv"
        result = run(PROBE.format(out=out.as_posix()), *args, env=env)
        assert out.is_file(), f"probe produced no output:\n{result.stdout}\n{result.stderr}"
        with out.open() as handle:
            return next(iter(csv.DictReader(handle)))

    return _probe


def tree(root: Path) -> set:
    """Every path under `root`, relative and posix-normalised. Empty when root is absent."""
    if not root.exists():
        return set()
    return {p.relative_to(root).as_posix() for p in root.rglob("*")} | {"."}


def homes(root: Path) -> list:
    return sorted(p for p in root.rglob("*-home") if p.is_dir())


def events(result) -> list:
    """The [TEST_EVENT] JSON objects on stdout/stderr, in order."""
    found = []
    for line in (result.stdout + result.stderr).splitlines():
        match = re.search(r"\{.*\"event\".*\}", line)
        if match:
            found.append(json.loads(match.group(0)))
    return found


# -----------------------------------------------------------------------------
# Reclaim: a run gives back exactly what it created
#


def test_run_created_root_and_ancestors_are_reclaimed(run, tmp_path):
    """PrepareTempDir must not materialize $ROOT behind RecordAndCreateLevels' back."""
    root = tmp_path / "a" / "b" / "root"
    assert run(TRIVIAL, "--temp-dir-root", str(root)).returncode == 0
    assert not (tmp_path / "a").exists(), f"leaked: {tree(tmp_path)}"


def test_preexisting_root_is_kept(run, tmp_path):
    """Reclaim stops at a root the run did not create."""
    root = tmp_path / "root"
    root.mkdir()
    assert run(TRIVIAL, "--temp-dir-root", str(root)).returncode == 0
    assert root.is_dir()
    assert tree(root) == {"."}, f"run root not reclaimed: {tree(root)}"


def test_adopted_run_root_is_not_recursively_removed(run, tmp_path):
    """RemoveDirectory is always recursive, so reclaiming a root the run merely adopted would take a
    co-located batch's live tree with it. Reclaim is gated on having created the level."""
    root = tmp_path / "root"
    (root / "R" / "other_batch").mkdir(parents=True)
    (root / "R" / "other_batch" / "inflight.db").write_text("x")
    assert run(TRIVIAL, "--temp-dir-root", str(root), "--run-id", "R").returncode == 0
    assert (root / "R" / "other_batch" / "inflight.db").is_file(), f"adopted root reaped: {tree(root)}"


def test_run_id_off_does_not_delete_the_caller_supplied_root(run, tmp_path):
    """With run-id off the run root IS $ROOT; a naive reclaim deletes the caller's directory."""
    root = tmp_path / "root"
    root.mkdir()
    (root / "PRECIOUS.txt").write_text("x")
    assert run(TRIVIAL, "--temp-dir-root", str(root), "--temp-dir-run-id", "off").returncode == 0
    assert (root / "PRECIOUS.txt").is_file(), f"caller's root reaped: {tree(root)}"


def _fail_after_prepare(run, tmp_path, *args):
    """Fail startup once the tree exists: ValidateDataDirs runs after PrepareTempDir created it."""
    result = run(TRIVIAL, *args, "--data-dir", str(tmp_path / "data-a"), "--local-data-dir", str(tmp_path / "data-b"))
    assert result.returncode != 0, "invalid data-dir combination accepted"
    assert "local-data-dir must equal data-dir" in result.stdout + result.stderr


def test_startup_failure_after_prepare_still_reclaims(run, tmp_path):
    """A return past PrepareTempDir owes the tree its disposition, same as a completed run."""
    root = tmp_path / "root"
    _fail_after_prepare(run, tmp_path, "--temp-dir-root", str(root), "--run-id", "R", "--temp-dir-destroy", "always")
    assert not root.exists(), f"leaked after a startup failure: {tree(root)}"


def test_startup_failure_after_prepare_honours_destroy_never(run, tmp_path):
    """A failed startup is exactly when 'never' is asked to leave the tree for inspection."""
    root = tmp_path / "root"
    _fail_after_prepare(run, tmp_path, "--temp-dir-root", str(root), "--run-id", "R", "--temp-dir-destroy", "never")
    assert (root / "R").is_dir(), f"run root reaped despite destroy=never: {tree(root)}"
    assert homes(root), f"HOME reaped despite destroy=never: {tree(root)}"


def test_empty_root_is_rejected(run):
    """Path::FromString("") is ".", so an empty root must be caught before it parses -- otherwise
    it silently roots the whole tree at the cwd."""
    result = run(TRIVIAL, "--temp-dir-root", "")
    assert result.returncode != 0, "empty root accepted"
    assert "non-empty" in result.stdout + result.stderr


def test_empty_root_is_rejected_via_env(run):
    result = run(TRIVIAL, env={"DUCKDB_TEST_TEMP_DIR_ROOT": ""})
    assert result.returncode != 0, "empty root accepted via env"
    assert "non-empty" in result.stdout + result.stderr


def test_relative_root_stays_relative(probe):
    """TEMP_DIR is deliberately relative so machine paths stay out of compared output; Path
    round-tripping must not absolutise or otherwise renormalise it."""
    variables = probe(
        "--temp-dir-root", "rel-root", "--run-id", "R", "--temp-dir-run-id", "off", "--temp-dir-test-id", "off"
    )
    assert variables["temp_dir"] == "rel-root"
    assert Path(variables["temp_dir_absolute"]).is_absolute()


def test_trailing_separator_in_root_does_not_double_up(probe, tmp_path):
    plain = probe("--temp-dir-root", str(tmp_path / "b"), "--run-id", "R")
    slashed = probe("--temp-dir-root", str(tmp_path / "b") + "/", "--run-id", "R")
    assert plain["temp_dir"] == slashed["temp_dir"]
    assert "//" not in plain["temp_dir"]


def test_dot_segments_in_root_are_collapsed(probe, tmp_path):
    """Path folds . and .. at parse time, so TEMP_DIR_ROOT reports the resolved form. Desired, and
    pinned because it is user-visible -- string composition used to pass them through."""
    real = tmp_path / "bar"
    real.mkdir()
    noisy = tmp_path / "foo" / ".." / "bar" / "." / ""
    variables = probe("--temp-dir-root", str(noisy), "--run-id", "R")
    assert variables["temp_dir_root"] == str(real)
    assert ".." not in variables["temp_dir"]
    assert variables["temp_dir"] == f"{real}/R/_stdin_"


def test_unwritable_root_reports_instead_of_terminating(run, tmp_path):
    """PrepareTempDir's contract is to report startup failures through `error`; an escaping
    IOException from mkdir/rmdir used to abort the process instead."""
    denied = tmp_path / "ro"
    denied.mkdir()
    denied.chmod(0o500)
    try:
        result = run(TRIVIAL, "--temp-dir-root", str(denied / "root"))
        assert result.returncode != 0
        output = result.stdout + result.stderr
        assert "Failed to prepare temp directory" in output
        assert "terminate called" not in output
    finally:
        denied.chmod(0o700)


# -----------------------------------------------------------------------------
# HOME sandbox: follows the temp-dir dispositions, never inside {TEST_DIR}
#


def test_destroy_never_keeps_the_home_sandbox(run, tmp_path):
    """'keep everything for inspection' must include HOME -- it holds ~/.duckdb."""
    root = tmp_path / "root"
    root.mkdir()
    assert run(TRIVIAL, "--temp-dir-root", str(root), "--temp-dir-destroy", "never").returncode == 0
    assert homes(root), f"HOME reaped despite destroy=never: {tree(root)}"


def test_destroy_on_success_reaps_the_home_sandbox(run, tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    assert run(TRIVIAL, "--temp-dir-root", str(root), "--temp-dir-destroy", "on-success").returncode == 0
    assert not homes(root), f"HOME retained: {tree(root)}"


def test_home_sandbox_is_a_sibling_never_inside_the_test_dir(probe, tmp_path):
    """HOME inside a {TEST_DIR} would make ~/.duckdb an allowed_directories path."""
    root = tmp_path / "root"
    root.mkdir()
    variables = probe("--temp-dir-root", str(root), "--temp-dir-destroy", "never")
    (home,) = homes(root)
    test_dir = Path(variables["temp_dir_absolute"])
    assert home not in test_dir.parents and home != test_dir


def test_home_sandbox_follows_the_pinned_local_tree(run, tmp_path):
    """HOME is a local-only concern, so it lives with the local tree wherever that is pinned."""
    sweep = tmp_path / "sweep"
    assert (
        run(
            TRIVIAL,
            "--temp-dir-root",
            "az://bucket/tmp",
            "--local-temp-dir-root",
            str(sweep),
            "--temp-dir-destroy",
            "never",
        ).returncode
        == 0
    )
    assert homes(sweep), f"HOME did not follow the local tree: {tree(sweep)}"


# -----------------------------------------------------------------------------
# LOCAL_TEMP_DIR: a TEMP_DIR that is never remote
#


def test_local_temp_dir_is_temp_dir_when_the_root_is_local(probe, tmp_path):
    """The invariant with no exceptions: local primary => one tree, one create/reap path."""
    root = tmp_path / "root"
    root.mkdir()
    variables = probe("--temp-dir-root", str(root))
    assert variables["local_temp_dir"] == variables["temp_dir"]


def test_local_temp_dir_carries_the_test_id_level(probe, tmp_path):
    """It tracks TEMP_DIR in use and intent, so it is per-test, not per-run."""
    root = tmp_path / "root"
    root.mkdir()
    variables = probe("--temp-dir-root", str(root), "--temp-dir-test-id", "on")
    assert variables["test_id"]
    assert variables["local_temp_dir"].endswith(variables["test_id"])


def test_remote_root_yields_a_distinct_local_tree(probe):
    """A remote root must neither crash nor leave LOCAL_TEMP_DIR remote."""
    variables = probe("--temp-dir-root", "az://bucket/tmp")
    assert variables["temp_dir"].startswith("az://")
    assert not variables["local_temp_dir"].startswith("az://")
    assert variables["local_temp_dir"] != variables["temp_dir"]
    assert Path(variables["local_temp_dir"]).is_absolute()
    assert variables["local_temp_dir"].endswith(variables["test_id"])


def test_remote_root_does_not_crash(run):
    """Regression: TEMP_DIR_ABSOLUTE used to JoinPath a scheme'd URI onto the cwd and throw."""
    result = run(TRIVIAL, "--temp-dir-root", "az://bucket/tmp")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "cannot join incompatible paths" not in result.stdout + result.stderr


def test_remote_root_reclaims_the_local_tree(run, duckdb_root):
    """The local tree is local, so the remote clamp must not exempt it from reclaim. Scoped to this
    run's own id: the default local root is shared, so a before/after snapshot of it would measure
    whatever else is running -- including this file's other tests, and anything under pytest -n."""
    local_root = duckdb_root / "duckdb_unittest_tempdir"
    run_id = "local-tree-reclaim"
    assert run(TRIVIAL, "--temp-dir-root", "az://bucket/tmp", "--run-id", run_id).returncode == 0
    assert not (local_root / run_id).exists(), "local tree run root leaked"
    assert not (local_root / f"{run_id}-home").exists(), "local tree HOME sandbox leaked"


def test_local_temp_dir_root_redirects_the_local_tree(probe, tmp_path):
    """Without it the local tree lands in the source tree, which is the reason to want it."""
    sweep = tmp_path / "sweep"
    variables = probe("--temp-dir-root", "az://bucket/tmp", "--local-temp-dir-root", str(sweep))
    assert variables["local_temp_dir"].startswith(str(sweep))
    assert variables["temp_dir"].startswith("az://")
    assert "duckdb_unittest_tempdir" not in variables["local_temp_dir"]


@pytest.mark.parametrize("primary", ["unset", "local"])
def test_local_temp_dir_root_is_rejected_against_a_local_primary(run, tmp_path, primary):
    """LOCAL_TEMP_DIR and TEMP_DIR must never be two different local dirs -- a local primary is its
    own LOCAL_TEMP_DIR, so pinning the local tree elsewhere fails rather than diverging."""
    args = ["--local-temp-dir-root", str(tmp_path / "sweep")]
    if primary == "local":
        (tmp_path / "root").mkdir()
        args += ["--temp-dir-root", str(tmp_path / "root")]
    result = run(TRIVIAL, *args)
    assert result.returncode != 0
    assert "conflicts with a local" in result.stdout + result.stderr
    assert not (tmp_path / "sweep").exists()


def test_local_temp_dir_root_matching_the_primary_is_accepted(probe, tmp_path):
    """Naming the same local root twice is redundant, not a contradiction."""
    root = tmp_path / "root"
    root.mkdir()
    variables = probe("--temp-dir-root", str(root), "--local-temp-dir-root", str(root))
    assert variables["local_temp_dir"] == variables["temp_dir"]
    assert variables["temp_dir"].startswith(str(root))


def test_local_temp_dir_root_rejects_a_remote_value(run):
    result = run(TRIVIAL, "--local-temp-dir-root", "az://bucket/x")
    assert result.returncode != 0
    assert "must be a local path" in result.stdout + result.stderr


# -----------------------------------------------------------------------------
# LOCAL_DATA_DIR
#


def test_local_data_dir_defaults_to_data_dir(probe):
    variables = probe()
    assert variables["local_data_dir"] == variables["data_dir"]


def test_local_data_dir_diverging_from_a_local_data_dir_is_rejected(run, tmp_path):
    """Same rule as --local-temp-dir-root: a local DATA_DIR is already its own LOCAL_DATA_DIR."""
    result = run(TRIVIAL, "--local-data-dir", str(tmp_path / "elsewhere"))
    assert result.returncode != 0
    assert "must equal data-dir" in result.stdout + result.stderr


def test_local_data_dir_backs_a_remote_data_dir(probe, tmp_path):
    local = tmp_path / "local-data"
    variables = probe("--data-dir", "az://bucket/data", "--local-data-dir", str(local))
    assert variables["data_dir"].startswith("az://")
    assert variables["local_data_dir"] == str(local)


def test_local_data_dir_rejects_a_remote_value(run):
    result = run(TRIVIAL, "--local-data-dir", "az://bucket/x")
    assert result.returncode != 0
    assert "must be a local path" in result.stdout + result.stderr


# -----------------------------------------------------------------------------
# [TEST_EVENT] stream
#


def test_begin_and_end_report_the_same_temp_dir(run, tmp_path):
    """`test-env TEMP_DIR <default>` resolves to the config's run-root value and overwrites the
    runner's per-test one, so reading the live map at end time made the two events disagree."""
    body = """# name: rewrite.test
# group: [compat]

test-env TEMP_DIR /unused-default

query I
SELECT 1
----
1
"""
    result = run(body, "--temp-dir-root", str(tmp_path / "b"), "--run-id", "R", "--emit-test-events")
    emitted = events(result)
    assert [e["event"] for e in emitted] == ["begin", "execution-begin", "execution-complete", "end"], emitted
    assert emitted[0]["version"] == 2
    assert all(e["temp_dir"] == emitted[0]["temp_dir"] for e in emitted)
    assert emitted[0]["temp_dir"].endswith("/R/_stdin_"), emitted[0]["temp_dir"]


def test_events_carry_the_driver_supplied_prefix(run, tmp_path):
    root = tmp_path / "driver-root"
    result = run(TRIVIAL, "--temp-dir-root", str(root), "--emit-test-events")
    for event in events(result):
        assert event["temp_dir"].startswith(str(root))


@pytest.mark.parametrize("body", [TRIVIAL, TRIVIAL + "\nendloop\n"])
def test_execution_complete_precedes_cleanup_hang(unittest_bin, duckdb_root, tmp_path, body):
    capture = tmp_path / "stderr"
    with capture.open("w") as stderr:
        proc = subprocess.Popen(
            [
                str(unittest_bin),
                "--test-dir",
                str(duckdb_root),
                "--stdin",
                "--emit-test-events",
                "--temp-dir-root",
                str(tmp_path / "temp"),
                "--on-cleanup",
                "SELECT sum(sin(a.i + b.i)) FROM range(1000000000) a(i), range(1000000000) b(i)",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
            text=True,
        )
        try:
            proc.communicate(body, timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
        else:
            pytest.fail(f"cleanup did not hang:\n{capture.read_text()}")
    emitted = events(subprocess.CompletedProcess([], proc.returncode, "", capture.read_text()))
    assert [e["event"] for e in emitted] == ["begin", "execution-begin", "execution-complete"], emitted
    assert emitted[2]["status"] == ("ok" if body == TRIVIAL else "error")


@pytest.mark.parametrize(
    "mode", ["body", "init_script", "init_query", "cleanup_script", "cleanup_query", "caught_error", "worker_error"]
)
def test_timeout_policy_with_real_lifecycle(unittest_bin, duckdb_root, tmp_path, mode):
    body = TRIVIAL
    flags = ["--test-dir", str(duckdb_root), "--temp-dir-root", str(tmp_path / "temp"), "--max-test-threads", "2"]
    if mode == "body":
        body += "\nsleep 60 second\n"
    elif mode in ("cleanup_script", "init_script"):
        script = tmp_path / "hook.test"
        script.write_text("sleep 60 second\n")
        flags += ["--cleanup-sqllogic" if mode == "cleanup_script" else "--init-sqllogic", str(script)]
    elif mode in ("cleanup_query", "caught_error", "init_query"):
        flags += [
            "--on-init" if mode == "init_query" else "--on-cleanup",
            "SELECT sum(sin(a.i + b.i)) FROM range(1000000000) a(i), range(1000000000) b(i)",
        ]
        if mode == "caught_error":
            body += "\nendloop\n"
    else:
        body += """
concurrentloop i 0 2

onlyif i=0
statement ok named_connection
SELECT 1

onlyif i=1
sleep 60 second

endloop
"""
    test = tmp_path / "probe.test"
    test.write_text(body)
    test_list = tmp_path / "tests"
    test_list.write_text(str(test) + "\n")
    result = subprocess.run(
        [
            sys.executable,
            str(duckdb_root / "scripts/ci/run_tests.py"),
            str(unittest_bin),
            "--test-list",
            str(test_list),
            "--test-flags",
            shlex.join(flags),
            "--batch-size",
            "1",
            "--batch-timeout",
            "2",
            "--timeout-policy",
            "warn",
            "--max-retries",
            "0",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        env={
            key: value
            for key, value in os.environ.items()
            if key != "GITHUB_STEP_SUMMARY" and not key.startswith("DUCKDB_TEST_")
        },
    )
    assert result.returncode == (0 if mode == "body" else 1), result.stdout + result.stderr
    if mode == "body":
        assert "0 passed, 0 skipped, 1 timed out" in result.stdout
    else:
        assert "0 passed, 1 failed" in result.stdout
        assert "error: timeout (2s)" in result.stdout
        assert "warn: timeout" not in result.stdout
        if mode == "worker_error":
            assert "Named connections not supported in parallel loop" in result.stdout


# -----------------------------------------------------------------------------
# --env-passthrough
#


def test_env_passthrough_substitutes_without_require_env(run):
    body = """# name: probe.test
# group: [compat]

query I
SELECT '{MY_THING}'
----
hello
"""
    result = run(body, "--env-passthrough", "MY_THING", env={"MY_THING": "hello"})
    assert result.returncode == 0, result.stdout + result.stderr


def test_env_passthrough_satisfies_require_env(run):
    body = """# name: probe.test
# group: [compat]

require-env MY_THING hello

query I
SELECT '{MY_THING}'
----
hello
"""
    result = run(body, "--env-passthrough", "MY_THING", env={"MY_THING": "hello"})
    assert result.returncode == 0, result.stdout + result.stderr


def test_env_passthrough_missing_var_fails_the_whole_invocation(unittest_bin):
    """Unlike require-env's per-test skip, this is loud and total."""
    env = {k: v for k, v in os.environ.items() if k != "MY_THING"}
    result = subprocess.run(
        [str(unittest_bin), "--env-passthrough", "MY_THING"], capture_output=True, text=True, env=env, timeout=300
    )
    assert result.returncode != 0
    assert "missing from the environment: MY_THING" in result.stdout + result.stderr


@pytest.mark.parametrize("reserved", ["TEMP_DIR", "DATA_DIR", "RUN_ID", "LOCAL_TEMP_DIR"])
def test_env_passthrough_rejects_reserved_names(run, reserved):
    """Passing one through would silently shadow the value the runner resolved."""
    result = run(TRIVIAL, "--env-passthrough", reserved, env={reserved: "/hijacked"})
    assert result.returncode != 0
    assert "reserved by the test runner" in result.stdout + result.stderr


def test_env_passthrough_accumulates_across_forms(run):
    body = """# name: probe.test
# group: [compat]

query II
SELECT '{ONE}', '{TWO}'
----
1	2
"""
    result = run(body, "--env-passthrough", "ONE", "--env-passthrough", "TWO", env={"ONE": "1", "TWO": "2"})
    assert result.returncode == 0, result.stdout + result.stderr


# -----------------------------------------------------------------------------
# Option surface: CLI, DUCKDB_TEST_<NAME> and --test-config must agree
#


def test_auto_run_id_is_compact_uuid(probe, tmp_path):
    """Keep full UUID entropy without consuming path space on a timestamp or mnemonic."""
    identifiers = [probe("--temp-dir-root", str(tmp_path / "root"))["run_id"] for _ in range(2)]
    for identifier in identifiers:
        random_id = uuid.UUID(identifier)
        assert str(random_id) == identifier
        assert random_id.version == 4
        assert random_id.variant == uuid.RFC_4122
    assert identifiers[0] != identifiers[1]


def test_run_id_via_cli_env_and_config_agree(probe, tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    config = tmp_path / "cfg.json"
    config.write_text(json.dumps({"temp_dir_root": str(root), "run_id": "from-config"}))

    via_cli = probe("--temp-dir-root", str(root), "--run-id", "pinned")
    via_env = probe(env={"DUCKDB_TEST_TEMP_DIR_ROOT": str(root), "DUCKDB_TEST_RUN_ID": "pinned"})
    via_config = probe("--test-config", str(config))

    assert via_cli["run_id"] == via_env["run_id"] == "pinned"
    assert via_config["run_id"] == "from-config"
    assert via_cli["temp_dir"] == via_env["temp_dir"]


def test_cli_beats_env(probe, tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    variables = probe("--run-id", "from-cli", "--temp-dir-root", str(root), env={"DUCKDB_TEST_RUN_ID": "from-env"})
    assert variables["run_id"] == "from-cli"


@pytest.mark.parametrize(
    "flag,value,expected",
    [
        ("--temp-dir-destroy", "bogus", "never, on-success, always"),
        ("--temp-dir-run-id", "maybe", "on, off"),
        ("--database-destroy", "bogus", "on, off, on-success"),
    ],
)
def test_invalid_disposition_is_rejected(run, flag, value, expected):
    result = run(TRIVIAL, flag, value)
    assert result.returncode != 0
    assert expected in result.stdout + result.stderr


# -----------------------------------------------------------------------------
# Ownership: a run configures, creates and reclaims only its own
#


def test_inherited_skip_config_does_not_replace_temp_options(probe, tmp_path, duckdb_root):
    """A config loaded solely to harvest another's `skip_tests` must not execute the option table's
    process-global side effects, or the inherited root silently replaces the selected one."""
    inherited = tmp_path / "inherited.json"
    inherited.write_text(json.dumps({"temp_dir_root": str(tmp_path / "inherited-root")}))
    outer = tmp_path / "outer.json"
    # inherit_skip_tests resolves against the binary's cwd (--test-dir), so keep the path relative:
    # joining an absolute one onto the cwd throws.
    outer.write_text(
        json.dumps(
            {
                "temp_dir_root": str(tmp_path / "selected-root"),
                "inherit_skip_tests": os.path.relpath(inherited, duckdb_root),
            }
        )
    )
    variables = probe("--test-config", str(outer))
    assert variables["temp_dir_root"] == str(tmp_path / "selected-root")


@pytest.mark.parametrize("name", ["TEMP_DIR", "LOCAL_TEMP_DIR", "DATA_DIR", "LOCAL_DATA_DIR", "RUN_ID"])
def test_config_test_env_cannot_override_a_reserved_name(run, tmp_path, name):
    """Engine-resolved variables are reserved against every external input. A `test_env` entry naming
    one must abort at startup rather than silently shadow the value the runner computed."""
    config = tmp_path / "cfg.json"
    config.write_text(json.dumps({"test_env": [{"env_name": name, "env_value": "/hijacked"}]}))
    result = run(TRIVIAL, "--test-config", str(config))
    assert result.returncode != 0
    output = result.stdout + result.stderr
    assert "reserved by the test runner" in output and name in output


def test_config_test_env_allows_its_own_names(probe, tmp_path):
    """The reservation is only over runner-resolved names -- a suite's own variables still work."""
    config = tmp_path / "cfg.json"
    config.write_text(json.dumps({"test_env": [{"env_name": "MY_SUITE_THING", "env_value": "ok"}]}))
    variables = probe("--test-config", str(config))
    assert variables["temp_dir"]


def test_home_sandbox_the_run_did_not_create_is_kept(run, tmp_path):
    """HOME needs the same created/adopted tracking as the run and test levels -- batches co-located
    under a fixed --run-id share one HOME path, so a reaping run can take a live one with it."""
    root = tmp_path / "root"
    (root / "R").mkdir(parents=True)
    home = root / "R-home"
    home.mkdir()
    (home / "marker").write_text("x")
    assert run(TRIVIAL, "--temp-dir-root", str(root), "--run-id", "R").returncode == 0
    assert (home / "marker").is_file(), f"adopted HOME reaped: {tree(root)}"


def test_per_test_dir_is_not_recreated_after_cleanup(run, tmp_path):
    """DestroyTestTempDir runs while the runner is alive, so the destructor's loaded-database
    ownership check must use a path snapshotted at construction -- resolving it there calls a
    MATERIALIZING accessor and recreates the leaf just reclaimed. Only observable once an adopted
    run root is correctly retained."""
    root = tmp_path / "root"
    (root / "R").mkdir(parents=True)
    result = run(TRIVIAL, "--temp-dir-root", str(root), "--run-id", "R", "--initial-db", "{TEST_DIR}/db")
    assert result.returncode == 0, result.stdout + result.stderr
    leftovers = sorted(p.name for p in (root / "R").iterdir())
    assert leftovers == [], f"per-test dir recreated after cleanup: {leftovers}"


@pytest.mark.parametrize("flag", ["-f", "--input-file"])
@pytest.mark.parametrize("with_config", [False, True])
@pytest.mark.parametrize("pattern", [False, True])
@pytest.mark.parametrize("shadow_list", [False, True])
def test_input_file_invocation_directory(unittest_bin, tmp_path, flag, with_config, pattern, shadow_list):
    working_dir = tmp_path / "suite"
    tests = working_dir / "test" / "sql"
    tests.mkdir(parents=True)
    body = TRIVIAL
    args = [str(unittest_bin.resolve()), "--test-dir", "suite", flag, "tests.txt"]
    if with_config:
        (tmp_path / "config.json").write_text(json.dumps({"on_init": "CREATE MACRO input_file_probe() AS 42"}))
        args.extend(["--test-config", "config.json"])
        body = body.replace("SELECT 42", "SELECT input_file_probe()")
    (tests / "probe.test").write_text(body)
    (tmp_path / "tests.txt").write_text("test/sql/*\n" if pattern else "test/sql/probe.test\n")
    if shadow_list:
        (working_dir / "tests.txt").write_text("suite/test/sql/missing.test\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_TEST_")}
    result = subprocess.run(args, cwd=tmp_path, env=env, capture_output=True, text=True, timeout=60)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "1 test case" in output, output
