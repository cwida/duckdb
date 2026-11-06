#!/usr/bin/env python3
"""
Ground-truth test for DuckDB's I/O profiling metrics.

DuckDB reports `io.total_bytes_read` / `io.total_bytes_written` profiling metrics.
These are only correct when a QueryContext is threaded down to the FileHandle
read/write calls; many code paths drop it and silently under- (or over-) count.

This harness measures the REAL bytes read/written by the process at the libc
syscall boundary using an interposition shim (io_shim.c), independent of
DuckDB's own instrumentation, and compares that ground truth against the
reported metrics. A mismatch is a hard failure (no tolerance).

Known-wrong metrics are listed in io_metrics_config.json with a "skip" reason
(the QueryContext-not-passed bugs). Those are reported but do not fail the run;
if a skipped test starts matching, the harness flags that the skip can be removed.

Usage:
    test/io_metrics/run_io_metrics_test.py [--duckdb PATH] [--config PATH]
                                           [--filter SUBSTR] [--keep] [--verbose]

Exit code is non-zero if any non-skipped test mismatches (or errors).
Requires a platform where library interposition works against a locally built
duckdb binary: macOS (DYLD_INSERT_LIBRARIES) or Linux/glibc (LD_PRELOAD).
"""
import argparse
import glob
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def log(msg=""):
    print(msg, flush=True)


# --------------------------------------------------------------------------- #
# Locating / building the pieces
# --------------------------------------------------------------------------- #
def find_duckdb(explicit):
    if explicit:
        if not os.path.exists(explicit):
            sys.exit(f"error: --duckdb '{explicit}' does not exist")
        return explicit
    for build in ("release", "reldebug", "debug"):
        cand = os.path.join(REPO, "build", build, "duckdb")
        if os.path.exists(cand):
            return cand
    sys.exit(
        "error: could not find a built duckdb binary under build/{release,reldebug,debug}; "
        "build one (e.g. `make release`) or pass --duckdb PATH"
    )


def build_shim(workdir):
    """Compile io_shim.c for this platform; return (lib_path, inject_env_var)."""
    src = os.path.join(HERE, "io_shim.c")
    system = platform.system()
    if system == "Darwin":
        lib = os.path.join(workdir, "io_shim.dylib")
        cmd = ["cc", "-O2", "-dynamiclib", "-o", lib, src]
        inject = "DYLD_INSERT_LIBRARIES"
    elif system == "Linux":
        lib = os.path.join(workdir, "io_shim.so")
        cmd = ["cc", "-O2", "-shared", "-fPIC", "-o", lib, src, "-ldl"]
        inject = "LD_PRELOAD"
    else:
        sys.exit(f"error: unsupported platform '{system}'; this test needs macOS or Linux")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"error: failed to compile io_shim.c:\n{res.stderr}")
    return lib, inject


# --------------------------------------------------------------------------- #
# Running DuckDB
# --------------------------------------------------------------------------- #
PROFILING_PRELUDE = [
    "PRAGMA enable_profiling='json';",
    "SET tracked_metrics=['io.total_bytes_read','io.total_bytes_written'];",
    # Disable the external file cache so every metric-tracked read corresponds to a real OS read
    # (a cache hit does no syscall I/O but is still counted by the metric, which would be
    # nondeterministic under the no-tolerance comparison).
    "SET enable_external_file_cache=false;",
    # Single-threaded so parquet's async prefetcher issues a deterministic set of coalesced reads
    # (under multi-thread load the real OS byte count can vary slightly run-to-run).
    "SET threads=1;",
]


def run_prep(duckdb, statements):
    """Create input files, un-instrumented (these bytes must not be counted)."""
    if not statements:
        return
    script = "".join(s.rstrip(";") + ";\n" for s in statements)
    res = subprocess.run([duckdb, "-batch", ":memory:"], input=script, text=True, capture_output=True)
    if res.returncode != 0:
        raise RuntimeError(f"prep failed: {res.stderr or res.stdout}")


def read_io_metric(path):
    """Pull io.total_bytes_{read,written} out of a profiling json file (0 if absent)."""
    try:
        with open(path) as f:
            io = json.load(f).get("io", {})
    except (OSError, ValueError):
        return 0, 0
    return int(io.get("total_bytes_read", 0) or 0), int(io.get("total_bytes_written", 0) or 0)


def run_measure(duckdb, statements, include, exclude, env_inject, shim_lib, workdir, database, settings):
    """
    Run the instrumented statements and return (reported_read, reported_written,
    os_read, os_written). os_* is the shim's syscall-level ground truth; reported_*
    is the sum of DuckDB's metric across the statements. `database` is the database
    the CLI opens (":memory:" for external-file tests, or a file for storage tests).
    """
    os_out = os.path.join(workdir, "os.json")
    env = dict(os.environ)
    env["IOSHIM_INCLUDE"] = ",".join(include)
    if exclude:
        env["IOSHIM_EXCLUDE"] = ",".join(exclude)
    env["IOSHIM_OUT"] = os_out
    env[env_inject] = shim_lib

    prelude = PROFILING_PRELUDE + [s.rstrip(";") + ";" for s in settings]
    rep_r = rep_w = 0
    if len(statements) == 1:
        # Single statement: simple one-shot, one profiling file. This is the exact
        # path used by the clean (passing) external-file cases.
        prof = os.path.join(workdir, "prof.json")
        script = "\n".join(prelude + [f"PRAGMA profiling_output='{prof}';", statements[0].rstrip(";") + ";"])
        res = subprocess.run([duckdb, "-batch", database], input=script, text=True, capture_output=True, env=env)
        if res.returncode != 0:
            raise RuntimeError(f"measure failed: {res.stderr or res.stdout}")
        rep_r, rep_w = read_io_metric(prof)
    else:
        # Multiple statements (e.g. CREATE/CHECKPOINT): drive them one at a time over
        # stdin, reading each statement's profile before the next pragma overwrites
        # it, and sum the reported metric across statements.
        rep_r, rep_w = _run_measure_multi(duckdb, statements, env, workdir, database, prelude)

    os_r, os_w = read_io_metric_os(os_out)
    return rep_r, rep_w, os_r, os_w


def read_io_metric_os(path):
    with open(path) as f:
        d = json.load(f)
    return int(d["os_bytes_read"]), int(d["os_bytes_written"])


def _run_measure_multi(duckdb, statements, env, workdir, database, prelude):
    proc = subprocess.Popen(
        [duckdb, "-batch", database],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        bufsize=1,
    )

    def send(line):
        proc.stdin.write(line + "\n")
        proc.stdin.flush()

    for line in prelude:
        send(line)

    rep_r = rep_w = 0
    for i, stmt in enumerate(statements):
        prof = os.path.join(workdir, f"stmt_{i}.json")
        send(f"PRAGMA profiling_output='{prof}';")
        send(stmt.rstrip(";") + ";")
        sentinel = f"__IOSHIM_DONE_{i}__"
        send(f".print {sentinel}")
        while True:
            out = proc.stdout.readline()
            if out == "" or sentinel in out:
                break
        r, w = read_io_metric(prof)
        rep_r += r
        rep_w += w
    send(".quit")
    proc.wait()
    return rep_r, rep_w


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description="Compare DuckDB io.* metrics against real OS I/O.")
    ap.add_argument("--duckdb", help="path to the duckdb binary (default: build/{release,reldebug,debug}/duckdb)")
    ap.add_argument("--config", default=os.path.join(REPO, "test", "configs", "io_metrics.json"))
    ap.add_argument("--filter", help="only run tests whose name contains this substring")
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    ap.add_argument("--verbose", action="store_true", help="print per-test byte counts even on pass")
    args = ap.parse_args()

    duckdb = find_duckdb(args.duckdb)
    with open(args.config) as f:
        config = json.load(f)
    tests = config["tests"]
    if args.filter:
        tests = [t for t in tests if args.filter in t["name"]]
    if not tests:
        sys.exit("error: no tests match --filter")

    # realpath: on macOS the temp dir is a symlink (/var -> /private/var) and DuckDB
    # canonicalizes paths it opens, so the shim's path-prefix match must use the real path.
    root = os.path.realpath(tempfile.mkdtemp(prefix="io_metrics_"))
    shim_lib, env_inject = build_shim(root)

    log(f"duckdb : {duckdb}")
    log(f"shim   : {shim_lib} (via {env_inject})")
    log(f"scratch: {root}")
    log("")

    failures, skipped, passed, stale_skips = [], [], [], []

    for t in tests:
        name = t["name"]
        workdir = os.path.join(root, name)
        os.makedirs(workdir, exist_ok=True)

        def subst(s):
            return s.replace("{dir}", workdir)

        include = [subst(p) for p in t["include"]]
        exclude = [subst(p) for p in t.get("exclude", [])]
        checks = t.get("check", ["read", "write"])
        skip_reason = t.get("skip")
        database = subst(t["default_db"]) if t.get("default_db") else ":memory:"

        try:
            run_prep(duckdb, [subst(s) for s in t.get("prep", [])])
            rep_r, rep_w, os_r, os_w = run_measure(
                duckdb,
                [subst(s) for s in t["measure"]],
                include,
                exclude,
                env_inject,
                shim_lib,
                workdir,
                database,
                t.get("settings", []),
            )
        except Exception as e:  # noqa: BLE001 - report any test error uniformly
            if skip_reason:
                skipped.append(name)
                log(f"SKIP {name}  (errored, skip reason recorded)\n     {skip_reason}\n     ({e})")
            else:
                failures.append(name)
                log(f"FAIL {name}  (error while running)\n     {e}")
            continue

        mism = []
        if "read" in checks and os_r != rep_r:
            mism.append(("read", os_r, rep_r))
        if "write" in checks and os_w != rep_w:
            mism.append(("write", os_w, rep_w))

        detail = f"read os={os_r} rep={rep_r} | write os={os_w} rep={rep_w}"

        if skip_reason:
            if mism:
                skipped.append(name)
                log(f"SKIP {name}  ({detail})")
                log(f"     {skip_reason}")
            else:
                # The known bug appears fixed: the metric now matches ground truth.
                stale_skips.append(name)
                log(f"SKIP?{name}  now MATCHES ground truth ({detail})")
                log(f"     -> the bug appears fixed; remove the \"skip\" for this test in the config.")
        elif mism:
            failures.append(name)
            log(f"FAIL {name}  ({detail})")
            for dim, o, r in mism:
                log(f"     {dim}: OS measured {o} bytes but DuckDB reported {r}")
        else:
            passed.append(name)
            if args.verbose:
                log(f"PASS {name}  ({detail})")
            else:
                log(f"PASS {name}")

    log("")
    log(
        f"summary: {len(passed)} passed, {len(failures)} failed, "
        f"{len(skipped)} skipped (known-wrong){', ' + str(len(stale_skips)) + ' stale skip(s)' if stale_skips else ''}"
    )

    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        log(f"scratch kept at {root}")

    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
