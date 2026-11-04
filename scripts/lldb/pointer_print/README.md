# LLDB Smart Pointer Formatting

This helper teaches LLDB how to display DuckDB smart pointers as their pointee
instead of as wrapper internals.

It targets:

- `duckdb::unique_ptr<T>`
- `duckdb::shared_ptr<T>`
- `duckdb::optional_ptr<T>`
- `duckdb::buffer_ptr<T>`
- `duckdb::arena_ptr<T>`
- `duckdb::unsafe_unique_ptr<T>`
- `duckdb::unsafe_shared_ptr<T>`
- `duckdb::unsafe_optional_ptr<T>`
- `duckdb::unsafe_arena_ptr<T>`

## Intended Usage

Import the script into LLDB:

```lldb
command script import <duckdb repository root>/scripts/lldb/pointer_print/pointer_print.py
```

After import, the script:

- defines and enables the `duckdb` LLDB type category
- adds a synthetic provider for the smart-pointer types above
- adds a summary that shows the pointee type and address
- registers `duckdb-p`
- aliases LLDB `p` to `duckdb-p`

## `duckdb-p`

`duckdb-p` evaluates an expression like LLDB's `expression` command, but keeps
the DuckDB smart-pointer formatter active at the root of the printed value.

Examples:

```lldb
duckdb-p my_unique_ptr
duckdb-p some_state.shared_buffer
duckdb-p /x my_optional_ptr
p my_shared_ptr
```

The `/x`-style prefix is forwarded to LLDB as `expression -f <format>`.

Expressions are evaluated once. Output and errors are returned through LLDB's
command result, so callers using `SBCommandInterpreter.HandleCommand` can capture
the printed value and check whether evaluation succeeded.

## What The Formatter Shows

- null pointers are rendered as `nullptr`
- non-null pointers show `<pointee type> @ <address>`
- expandable values expose the pointee's children instead of the pointer wrapper

This avoids depending on `operator*()` or `.get()` template symbols being
emitted in the binary.

## Rename Safety

The script derives its LLDB callback module name at import time, so the callback
registration stays aligned if the file is renamed later.
