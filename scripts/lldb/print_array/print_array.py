"""LLDB helper for printing arrays behind pointers.

Usage inside LLDB:
    command script import /path/to/duckdb/scripts/lldb/print_array/print_array.py
    print_array result_sel.sel_vector 1024

`print_array <path> <size>` only accepts:
    - a positive array size
    - a `path` expression that evaluates to a non-null pointer
"""

from __future__ import annotations

import argparse
import shlex
from types import SimpleNamespace


try:
    import lldb  # type: ignore
except ImportError:  # pragma: no cover - imported by LLDB at runtime
    lldb = None


MODULE_NAME = __name__.rsplit(".", 1)[-1]
COMMAND_NAME = "print_array"


def __lldb_init_module(debugger, _internal_dict):
    debugger.HandleCommand(
        "command script add --overwrite -h "
        "\"Print a fixed-size array behind a pointer expression\" "
        "-f {} {}".format(_callback_name("print_array"), COMMAND_NAME)
    )


def print_array(debugger, command, result, _internal_dict):
    parser = _create_parser()
    args = _parse_args(parser, command, result)
    if args is None:
        return
    if args.help:
        result.AppendMessage(parser.format_help().rstrip())
        return

    frame = _get_selected_frame(debugger, result)
    if frame is None:
        return

    size = _evaluate_positive_size(frame, args.size, result)
    if size is None:
        return

    value = frame.EvaluateExpression(args.path)
    if not value.IsValid():
        result.SetError("path {!r} did not evaluate to a value".format(args.path))
        return
    if not value.GetError().Success():
        result.SetError(
            "failed to evaluate path {!r}: {}".format(args.path, value.GetError().GetCString() or "<unknown error>")
        )
        return

    value_type = value.GetType()
    if not value_type.IsValid() or not value_type.IsPointerType():
        result.SetError("path {!r} must evaluate to a pointer".format(args.path))
        return

    pointer_value = value.GetValueAsUnsigned(0)
    if pointer_value <= 0:
        result.SetError("path {!r} must evaluate to a positive, non-null pointer".format(args.path))
        return

    pointee_type = value_type.GetPointeeType()
    if not pointee_type.IsValid():
        result.SetError("could not determine the pointee type for {!r}".format(args.path))
        return

    pointee_name = pointee_type.GetName()
    if not pointee_name:
        result.SetError("could not determine the pointee type name for {!r}".format(args.path))
        return

    expression = "*(({} (*)[{}])({}))".format(pointee_name, size, args.path)
    command_result = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand("expression -- {}".format(expression), command_result)

    if command_result.Succeeded():
        output = command_result.GetOutput().rstrip()
        if output:
            result.AppendMessage(output)
        return

    error_text = command_result.GetError().rstrip()
    if not error_text:
        error_text = "failed to print array"
    result.SetError(error_text)


def _create_parser():
    parser = argparse.ArgumentParser(
        prog=COMMAND_NAME,
        description="Print a fixed-size array behind a pointer expression.",
        add_help=False,
    )
    parser.add_argument("path")
    parser.add_argument("size")
    parser.add_argument("--help", action="store_true")
    return parser


def _parse_args(parser, command, result):
    try:
        tokens = shlex.split(command)
    except ValueError as error:
        result.SetError(str(error))
        return None
    if "--help" in tokens or "-h" in tokens:
        return SimpleNamespace(help=True)

    try:
        return parser.parse_args(tokens)
    except SystemExit:
        result.SetError(parser.format_usage().rstrip())
        return None


def _try_parse_positive_int(raw_value):
    try:
        value = int(raw_value, 10)
    except ValueError:
        return None

    if value <= 0:
        return None
    return value


def _evaluate_positive_size(frame, size_expression, result):
    literal_size = _try_parse_positive_int(size_expression)
    if literal_size is not None:
        return literal_size

    size_value = frame.EvaluateExpression(size_expression)
    if not size_value.IsValid():
        result.SetError("size must be a positive integer")
        return None
    if not size_value.GetError().Success():
        result.SetError("size must be a positive integer")
        return None

    if not size_value.GetType().GetTypeFlags() & lldb.eTypeIsInteger:
        result.SetError("size must be a positive integer")
        return None

    size = size_value.GetValueAsSigned(-1)
    if size <= 0:
        result.SetError("size must be a positive integer")
        return None
    return size


def _get_selected_frame(debugger, result):
    target = debugger.GetSelectedTarget()
    if not target.IsValid():
        result.SetError("no selected target")
        return None

    process = target.GetProcess()
    if not process.IsValid():
        result.SetError("no selected process")
        return None

    thread = process.GetSelectedThread()
    if not thread.IsValid():
        result.SetError("no selected thread")
        return None

    frame = thread.GetSelectedFrame()
    if not frame.IsValid():
        result.SetError("no selected frame")
        return None
    return frame


def _callback_name(name):
    return "{}.{}".format(MODULE_NAME, name)
