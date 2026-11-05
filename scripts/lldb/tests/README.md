# LLDB Helper Tests

Run from the repository root:

```sh
python3 scripts/lldb/tests/run_tests.py --unittest build/debug/test/unittest \
  --coverage-dir /tmp/duckdb-lldb-coverage
```

The runner compiles `values.cpp` against DuckDB's pointer headers in a temporary
directory, then runs Python `unittest` tests inside a fresh LLDB session. It uses
the supplied DuckDB unittest executable for the SQL fixture in `test/statements.test`.
No DuckDB rebuild is needed when only the Python helpers change.

Requirements:

- LLDB with Python scripting and permission to debug local child processes.
- A C++ compiler; override the default with `--cxx <compiler>`.
- A DuckDB unittest build with a resolvable `query_break` symbol and inspectable
  locals. The runner defaults to `build/reldebug/test/unittest`; use a debug build
  if optimization removes the hook or locals.

Use `--lldb <executable>` to select LLDB. The runner uses `--no-lldbinit` and does
not change personal debugger configuration. It returns a nonzero status if a test
fails, compilation fails, or LLDB cannot complete the suite.

Coverage uses Python's standard-library `trace` module inside LLDB, so it needs no
third-party Python packages. `--coverage-dir` writes annotated `.cover` files for
the four helper modules and prints line-coverage percentages. Omit the option to
run without tracing. This measures executed Python lines, not branch coverage or
coverage of the DuckDB C++ executable.

CI runs this suite in the **OSX Debug** job in
[OSX.yml](../../../.github/workflows/OSX.yml). After the existing release tests,
the job builds a separate debug executable with the symbols and locals needed by LLDB,
runs all helper tests, and uploads the annotated reports as `lldb-coverage-osx`,
including when tests fail. It respects the workflow's `skip_tests` input.
The LLDB debug build leaves compiler warnings non-fatal: Debug enables additional
conversion warnings beyond the existing release build's warning set.
Changes under `scripts/lldb/` select the OSX workflow on pull requests, including
when the reduced CI matrix is enabled.

Tests cover command-result output and errors, argument validation, actual smart
pointer and array values, single evaluation of print expressions, SQL filters and
JSON context, duplicate SQL hooks, watch lifecycle, breakpoint restoration, and
step-avoid setting restoration. Platform-specific formatter fallbacks and every
possible invalid LLDB object are not exhaustively exercised.
