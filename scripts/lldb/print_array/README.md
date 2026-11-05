# LLDB Pointer Array Printer

This helper adds a small LLDB command for printing a fixed-size array that lives
behind a pointer expression.

## Usage

Import the script:

```lldb
command script import <duckdb repository root>/scripts/lldb/print_array/print_array.py
```

Then call:

```lldb
print_array <path> <size>
```

Examples:

```lldb
print_array result_sel.sel_vector 1024
print_array row_ids count
print_array data_ptr standard_vector_size
```

## Arguments

- `<path>` must evaluate to a non-null pointer in the selected frame
- `<size>` must evaluate to a positive integer
- fractional sizes are rejected, and size expressions are evaluated once

`<size>` can be either:

- a decimal literal such as `1024`
- an expression LLDB can evaluate in the selected frame, such as `count`

Quote arguments containing spaces, for example:

```lldb
print_array "row_ids + offset" "count - offset"
```

The command cannot determine the allocation's bounds; keep the size within the
known valid element count.

## Behavior

The command:

- validates that a target, process, thread, and frame are selected
- evaluates the pointer expression
- checks that the result is a non-null pointer
- derives the pointee type
- prints the pointee as `(<type>[<size>])`

Internally it issues an LLDB expression equivalent to:

```c++
*((<pointee type> (*)[<size>])(<path>))
```

## Help

You can ask LLDB for the built-in usage text with:

```lldb
print_array --help
```
