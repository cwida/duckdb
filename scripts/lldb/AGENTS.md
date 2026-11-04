# LLDB Helpers for Agents

Use these Python scripts inside LLDB when investigating DuckDB code or a failing
sqllogictest. They provide SQL-level navigation and make C++ values easier to inspect.
Read the relevant helper's README and implementation before modifying it.

## Start a Session

Run LLDB from the repository root so test paths and test data resolve correctly.
Start with the `reldebug` build recommended in the root AGENTS.md:

```sh
lldb -- build/reldebug/test/unittest test/sql/order/test_limit.test
```

Import the helpers you need at the LLDB prompt:

```lldb
command script import scripts/lldb/sqllogictest_breakpoints/sql_break.py
command script import scripts/lldb/pointer_print/pointer_print.py
command script import scripts/lldb/print_array/print_array.py
command script import scripts/lldb/filter_boundschecks/filter_checks.py
```

These are LLDB plugins, not standalone Python programs. Imports apply to the current
debugger session; no `~/.lldbinit` changes are needed. Use absolute script paths if
LLDB was started from another directory.

The SQL helpers need the unittest executable, a stopped process, and sqllogictest
frames on thread 1. They use `query_break` in `test/sqlite/sqllogic_command.cpp`.
If that hook has no breakpoint locations or required locals are optimized out,
use `make debug` and `build/debug/test/unittest` for the investigation.
The printing and stepping helpers also work with the DuckDB shell executable.

## Find the SQL Behind a C++ Stop

After importing `sql_break.py`, stop at the first SQL hook:

```lldb
breakpoint set --name query_break --one-shot true
run
sql_current_statement
```

`sql_current_statement` prints the test file, line, statement kind, connection, SQL,
and active loop values. It also works from a deeper C++ breakpoint when the
sqllogictest caller is still on thread 1's stack.
Use `sql_current_statement --json` to capture these fields as a JSON object rather
than parsing the display text.

Use these commands to move through the test:

```lldb
sql_next_statement
sql_next_matching_statement --kind statement_error
sql_next_matching_statement --file test/sql/join --kind query --loop i=3
sql_next_matching_statement --connection con2
sql_next_matching_statement --sql "JOIN" --kind query
```

Choose filters that match the test being debugged. Supported filters are:

- `--file <substring>` and `--line <n>`.
- `--line-min <n>` and `--line-max <n>` for an inclusive range; do not combine these with `--line`.
- `--kind query|statement|statement_ok|statement_error`.
- `--connection <name>` for an exact connection-name match.
- `--sql <substring>` for a case-sensitive match against the expanded SQL.
- `--loop <name>` or `--loop <name>=<value>`; repeat for multiple loop constraints.

Filters combine with AND. Use the location reported by `sql_current_statement`
when choosing a line filter.

For repeated stops, install a persistent rule and advance to it:

```lldb
sql_watch_statement --kind query --loop i=3
sql_list_watches
sql_next_watch
sql_delete_watch
```

`sql_next_watch <id>` selects one rule; omitting the ID considers all installed
rules. `sql_delete_watch <id>` removes one, omitting the ID removes the most
recently added rule, and `sql_delete_watch all` removes all rules.

SQL watches are breakpoint rules, not memory watchpoints. Ordinary `continue`
does not stop on them. `sql_next_statement`, `sql_next_matching_statement`, and
`sql_next_watch` temporarily auto-continue other breakpoints and restore their
prior settings afterward. Once positioned at the desired SQL, use ordinary
`continue` or stepping to investigate it with C++ breakpoints.

These helpers assume thread 1 is executing the test; selecting a worker thread
does not change that assumption. See [sqllogictest_breakpoints/README.md](sqllogictest_breakpoints/README.md).

## Inspect Pointers and Arrays

Importing `pointer_print.py` enables the `duckdb` type category and aliases `p` to
`duckdb-p`. Use it to inspect a DuckDB smart pointer directly:

```lldb
duckdb-p my_unique_ptr
duckdb-p some_state.shared_buffer
duckdb-p /x my_optional_ptr
```

The formatter displays the pointee type and address, exposes its children, and
renders null pointers as `nullptr`. It reads wrapper storage without relying on
emitted `operator*()` or `.get()` symbols. It supports DuckDB's unique, shared,
optional, buffer, arena, and listed unsafe pointer types.
Use `expression -- <expr>` when you need LLDB's expression command directly.
See [pointer_print/README.md](pointer_print/README.md) for the supported types.

Use `print_array` for a raw pointer to multiple elements in the selected frame:

```lldb
print_array result_sel.sel_vector 8
print_array row_ids count
print_array "data_ptr + offset" "count - offset"
```

The pointer must be non-null and the size must evaluate to a positive integer.
Quote arguments containing spaces. Choose a small size within the known valid
element count: the helper cannot check the allocation's bounds.
See [print_array/README.md](print_array/README.md).

## Step Past Wrapper Checks

Importing `filter_checks.py` immediately adds step-avoid patterns for small
`optional_ptr`, `unique_ptr`, `shared_ptr`, and `vector` helper frames.

```lldb
duckdb-step-avoid-show
duckdb-step-avoid-disable
duckdb-step-avoid-enable
```

Disable the additions when investigating the wrappers or their checks themselves.
Disabling restores the regexp saved before enabling the additions.
See [filter_boundschecks/README.md](filter_boundschecks/README.md).

## Changing a Helper

Keep command syntax and import side effects documented in the corresponding README.
Preserve callback registration derived from `MODULE_NAME` so scripts can be renamed.
Validate behavior inside LLDB: a Python syntax check alone does not exercise LLDB's
API, expression evaluation, formatters, or breakpoint callbacks. For SQL navigation
changes, use a small sqllogictest and verify matching, watch deletion, and restoration
of other breakpoints' auto-continue settings.

Run the integration suite from the repository root:

```sh
python3 scripts/lldb/tests/run_tests.py --unittest build/debug/test/unittest \
  --coverage-dir /tmp/duckdb-lldb-coverage
```

The suite needs LLDB with Python support, a C++ compiler, and permission to launch
local processes under the debugger. Use a build with a resolvable `query_break`
symbol and inspectable locals. See [tests/README.md](tests/README.md) for coverage
details and runner options.
