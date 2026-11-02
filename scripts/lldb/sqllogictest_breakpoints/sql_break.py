"""LLDB helpers for DuckDB sqllogictest debugging."""

from __future__ import annotations

import argparse
import ast
import json
import lldb
import shlex


SQL_CURRENT_STATEMENT_COMMAND = "sql_current_statement"
SQL_NEXT_STATEMENT_COMMAND = "sql_next_statement"
SQL_NEXT_MATCHING_STATEMENT_COMMAND = "sql_next_matching_statement"
SQL_WATCH_STATEMENT_COMMAND = "sql_watch_statement"
SQL_LIST_WATCHES_COMMAND = "sql_list_watches"
SQL_DELETE_WATCH_COMMAND = "sql_delete_watch"
SQL_NEXT_WATCH_COMMAND = "sql_next_watch"
MODULE_NAME = __name__.rsplit(".", 1)[-1]

_NEXT_STATE = None
_NEXT_WATCH_STATE = None
_WATCHES = {}


def __lldb_init_module(debugger, _internal_dict):
    _register_command(
        debugger,
        SQL_CURRENT_STATEMENT_COMMAND,
        "Show the active DuckDB sqllogictest location and SQL",
        _callback_name("sql_current_statement"),
    )
    _register_command(
        debugger,
        SQL_NEXT_STATEMENT_COMMAND,
        "Continue until the next sqllogictest statement",
        _callback_name("sql_next_statement"),
    )
    _register_command(
        debugger,
        SQL_NEXT_MATCHING_STATEMENT_COMMAND,
        "Continue until the next matching sqllogictest statement",
        _callback_name("sql_next_matching_statement"),
    )
    _register_command(
        debugger,
        SQL_WATCH_STATEMENT_COMMAND,
        "Install a persistent sqllogictest-aware watch rule",
        _callback_name("sql_watch_statement"),
    )
    _register_command(
        debugger,
        SQL_LIST_WATCHES_COMMAND,
        "List installed sqllogictest-aware watch rules",
        _callback_name("sql_list_watches"),
    )
    _register_command(
        debugger,
        SQL_DELETE_WATCH_COMMAND,
        "Delete one installed sqllogictest-aware watch rule",
        _callback_name("sql_delete_watch"),
    )
    _register_command(
        debugger,
        SQL_NEXT_WATCH_COMMAND,
        "Continue until an installed sqllogictest watch rule matches",
        _callback_name("sql_next_watch"),
    )


def sql_current_statement(debugger, command, result, _internal_dict):
    option = command.strip()
    if option not in ("", "--json"):
        result.SetError("usage: {} [--json]".format(SQL_CURRENT_STATEMENT_COMMAND))
        return

    context, error = _get_current_statement_context(debugger.GetSelectedTarget())
    if error:
        result.SetError(error)
        return

    if option == "--json":
        result.AppendMessage(
            json.dumps({key: value for key, value in context.items() if key not in ("frame", "priority")})
        )
    else:
        result.AppendMessage(_format_statement_context(context))


def sql_next_statement(debugger, command, result, _internal_dict):
    if command.strip():
        result.SetError("usage: {}".format(SQL_NEXT_STATEMENT_COMMAND))
        return

    matcher = _default_matcher()
    error = _start_next_statement_search(debugger, matcher)
    if error:
        result.SetError(error)


def sql_next_matching_statement(debugger, command, result, _internal_dict):
    matcher, error = _parse_matcher_command(SQL_NEXT_MATCHING_STATEMENT_COMMAND, command)
    if error:
        result.SetError(error)
        return

    error = _start_next_statement_search(debugger, matcher)
    if error:
        result.SetError(error)


def sql_watch_statement(debugger, command, result, _internal_dict):
    matcher, error = _parse_matcher_command(SQL_WATCH_STATEMENT_COMMAND, command)
    if error:
        result.SetError(error)
        return

    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    if not process.IsValid():
        result.SetError("no selected process")
        return

    thread = process.GetThreadAtIndex(0)
    if not thread.IsValid():
        result.SetError("thread 1 not found")
        return

    watch_breakpoint = target.BreakpointCreateByName("query_break")
    if not watch_breakpoint.IsValid() or watch_breakpoint.GetNumLocations() == 0:
        if watch_breakpoint.IsValid():
            target.BreakpointDelete(watch_breakpoint.GetID())
        result.SetError("could not set a breakpoint on query_break")
        return

    watch_breakpoint.SetThreadIndex(1)
    watch_breakpoint.SetAutoContinue(True)
    watch_breakpoint.SetScriptCallbackFunction(_callback_name("_watch_statement_callback"))

    breakpoint_id = watch_breakpoint.GetID()
    _WATCHES[breakpoint_id] = {
        "matcher": matcher,
        "description": _describe_matcher(matcher),
    }

    result.AppendMessage(
        "Installed watch {} on query_break ({})".format(breakpoint_id, _WATCHES[breakpoint_id]["description"])
    )


def sql_list_watches(debugger, command, result, _internal_dict):
    if command.strip():
        result.SetError("usage: {}".format(SQL_LIST_WATCHES_COMMAND))
        return

    target = debugger.GetSelectedTarget()
    _cleanup_dead_watches(target)

    if not _WATCHES:
        result.AppendMessage("No sql watches installed.")
        return

    lines = []
    for breakpoint_id in sorted(_WATCHES):
        breakpoint = target.FindBreakpointByID(breakpoint_id)
        status = "enabled"
        if breakpoint.IsValid() and not breakpoint.IsEnabled():
            status = "disabled"
        lines.append("{}: {} [{}]".format(breakpoint_id, _WATCHES[breakpoint_id]["description"], status))
    result.AppendMessage("\n".join(lines))


def sql_delete_watch(debugger, command, result, _internal_dict):
    parser = argparse.ArgumentParser(prog=SQL_DELETE_WATCH_COMMAND, add_help=False)
    parser.add_argument("watch_id", nargs="?")

    try:
        args = parser.parse_args(shlex.split(command))
    except ValueError as error:
        result.SetError(str(error))
        return
    except SystemExit:
        result.SetError("usage: {} [id|all]".format(SQL_DELETE_WATCH_COMMAND))
        return

    target = debugger.GetSelectedTarget()
    _cleanup_dead_watches(target)

    if not _WATCHES:
        result.SetError("no sql watches installed")
        return

    if args.watch_id is None:
        breakpoint_id = max(_WATCHES)
    elif args.watch_id == "all":
        deleted_ids = sorted(_WATCHES)
        for watch_id in deleted_ids:
            target.BreakpointDelete(watch_id)
            del _WATCHES[watch_id]
        result.AppendMessage("Deleted all watches ({})".format(", ".join(str(watch_id) for watch_id in deleted_ids)))
        return
    else:
        try:
            breakpoint_id = int(args.watch_id)
        except ValueError:
            result.SetError("usage: {} [id|all]".format(SQL_DELETE_WATCH_COMMAND))
            return

    if breakpoint_id not in _WATCHES:
        result.SetError("watch {} not found".format(breakpoint_id))
        return

    target.BreakpointDelete(breakpoint_id)
    del _WATCHES[breakpoint_id]
    result.AppendMessage("Deleted watch {}".format(breakpoint_id))


def sql_next_watch(debugger, command, result, _internal_dict):
    parser = argparse.ArgumentParser(prog=SQL_NEXT_WATCH_COMMAND, add_help=False)
    parser.add_argument("watch_id", nargs="?", type=int)

    try:
        args = parser.parse_args(shlex.split(command))
    except ValueError as error:
        result.SetError(str(error))
        return
    except SystemExit:
        result.SetError("usage: {} [id]".format(SQL_NEXT_WATCH_COMMAND))
        return

    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    if not process.IsValid():
        result.SetError("no selected process")
        return
    if process.GetState() != lldb.eStateStopped:
        result.SetError("process must be stopped")
        return

    thread = process.GetThreadAtIndex(0)
    if not thread.IsValid():
        result.SetError("thread 1 not found")
        return

    _cleanup_dead_watches(target)
    if not _WATCHES:
        result.SetError("no sql watches installed")
        return

    selected_watch_ids = sorted(_WATCHES)
    if args.watch_id is not None:
        if args.watch_id not in _WATCHES:
            result.SetError("watch {} not found".format(args.watch_id))
            return
        selected_watch_ids = [args.watch_id]

    _restore_next_watch_state(target)

    saved_auto_continue = {}
    saved_watch_auto_continue = {}
    for breakpoint in _iter_breakpoints(target):
        breakpoint_id = breakpoint.GetID()
        if breakpoint_id in _WATCHES:
            breakpoint.SetThreadIndex(1)
            saved_watch_auto_continue[breakpoint_id] = breakpoint.GetAutoContinue()
            breakpoint.SetAutoContinue(breakpoint_id not in selected_watch_ids)
            continue
        saved_auto_continue[breakpoint_id] = breakpoint.GetAutoContinue()
        if breakpoint.IsEnabled():
            breakpoint.SetAutoContinue(True)

    global _NEXT_WATCH_STATE
    _NEXT_WATCH_STATE = {
        "saved_auto_continue": saved_auto_continue,
        "saved_watch_auto_continue": saved_watch_auto_continue,
        "selected_watch_ids": set(selected_watch_ids),
    }

    command_interpreter = debugger.GetCommandInterpreter()
    command_result = lldb.SBCommandReturnObject()
    command_interpreter.HandleCommand("process continue", command_result)
    if _NEXT_WATCH_STATE is not None:
        _restore_next_watch_state(target)

    if command_result.Succeeded():
        return

    result.SetError(command_result.GetError() or "process continue failed")


def _register_command(debugger, name, help_text, callback):
    debugger.HandleCommand('command script add --overwrite -h "{}" -f {} {}'.format(help_text, callback, name))


def _callback_name(function_name):
    return "{}.{}".format(MODULE_NAME, function_name)


def _default_matcher():
    return {
        "file": None,
        "line": None,
        "line_min": None,
        "line_max": None,
        "kind": None,
        "connection": None,
        "sql": None,
        "loops": [],
    }


def _parse_matcher_command(prog, command):
    parser = argparse.ArgumentParser(prog=prog, add_help=False)
    parser.add_argument("--file")
    parser.add_argument("--line", type=int)
    parser.add_argument("--line-min", type=int)
    parser.add_argument("--line-max", type=int)
    parser.add_argument("--kind", choices=("query", "statement", "statement_ok", "statement_error"))
    parser.add_argument("--connection")
    parser.add_argument("--sql")
    parser.add_argument("--loop", action="append", default=[])

    try:
        args = parser.parse_args(shlex.split(command))
    except ValueError as error:
        return None, str(error)
    except SystemExit:
        return None, _matcher_usage(prog)

    matcher = _default_matcher()
    matcher["file"] = args.file
    matcher["line"] = args.line
    matcher["line_min"] = args.line_min
    matcher["line_max"] = args.line_max
    matcher["kind"] = args.kind
    matcher["connection"] = args.connection
    matcher["sql"] = args.sql

    if matcher["line"] is not None:
        if matcher["line_min"] is not None or matcher["line_max"] is not None:
            return None, "--line cannot be combined with --line-min/--line-max"
    if matcher["line_min"] is not None and matcher["line_max"] is not None:
        if matcher["line_min"] > matcher["line_max"]:
            return None, "--line-min cannot be greater than --line-max"

    loops = []
    for loop_filter in args.loop:
        loop_name, loop_value, error = _parse_loop_filter(loop_filter)
        if error:
            return None, error
        loops.append({"name": loop_name, "value": loop_value})
    matcher["loops"] = loops
    return matcher, None


def _matcher_usage(prog):
    return (
        "usage: {} [--file <substring>] [--line <n>] [--line-min <n>] "
        "[--line-max <n>] [--kind query|statement|statement_ok|statement_error] [--connection <name>] "
        "[--sql <substring>] [--loop <name>[=<value>]]".format(prog)
    )


def _parse_loop_filter(loop_filter):
    if not loop_filter:
        return None, None, "loop filter cannot be empty"

    if "=" in loop_filter:
        loop_name, loop_value = loop_filter.split("=", 1)
        if not loop_name:
            return None, None, "loop filter must use <name>=<value>"
        if not loop_value:
            return None, None, "loop filter value cannot be empty"
        return loop_name, loop_value, None

    return loop_filter, None, None


def _start_next_statement_search(debugger, matcher):
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    if not process.IsValid():
        return "no selected process"
    if process.GetState() != lldb.eStateStopped:
        return "process must be stopped"

    thread = process.GetThreadAtIndex(0)
    if not thread.IsValid():
        return "thread 1 not found"

    _restore_next_state(target)

    helper_breakpoint = target.BreakpointCreateByName("query_break")
    if not helper_breakpoint.IsValid() or helper_breakpoint.GetNumLocations() == 0:
        if helper_breakpoint.IsValid():
            target.BreakpointDelete(helper_breakpoint.GetID())
        return "could not set a breakpoint on query_break"

    helper_breakpoint.SetThreadID(thread.GetThreadID())
    helper_breakpoint.SetScriptCallbackFunction(_callback_name("_next_matching_statement_callback"))

    saved_auto_continue = {}
    for breakpoint in _iter_breakpoints(target):
        if breakpoint.GetID() == helper_breakpoint.GetID():
            continue
        saved_auto_continue[breakpoint.GetID()] = breakpoint.GetAutoContinue()
        if breakpoint.IsEnabled():
            breakpoint.SetAutoContinue(True)

    global _NEXT_STATE
    _NEXT_STATE = {
        "helper_breakpoint_id": helper_breakpoint.GetID(),
        "saved_auto_continue": saved_auto_continue,
        "matcher": matcher,
    }

    command_interpreter = debugger.GetCommandInterpreter()
    command_result = lldb.SBCommandReturnObject()
    command_interpreter.HandleCommand("process continue", command_result)
    if _NEXT_STATE is not None:
        _restore_next_state(target)

    if command_result.Succeeded():
        return None

    return command_result.GetError() or "process continue failed"


def _get_current_statement_context(target):
    process = target.GetProcess()
    if not process.IsValid():
        return None, "no selected process"

    thread = process.GetThreadAtIndex(0)
    if not thread.IsValid():
        return None, "thread 1 not found"

    context = _find_sqllogictest_context(thread)
    if context is None:
        return None, "no sqllogictest frame found on thread 1"
    return context, None


def _find_sqllogictest_context(thread):
    best_match = None
    for frame in thread:
        function_name = frame.GetFunctionName() or ""
        frame_kind = _classify_frame(function_name)
        if frame_kind is None:
            continue

        context = {
            "frame": frame,
            "priority": frame_kind["priority"],
            "kind": frame_kind["kind"],
            "statement_expectation": None,
            "file_name": None,
            "query_line": None,
            "sql_text": None,
            "connection_name": None,
            "running_loops": [],
            "loop_values": {},
        }

        if frame_kind["style"] == "execute_internal":
            context["file_name"] = _read_cpp_string(frame, "this->file_name")
            context["query_line"] = _read_int(frame, "this->query_line")
            context["sql_text"] = _read_cpp_string(frame, "context.sql_query")
            context["connection_name"] = _read_cpp_string(frame, "this->connection_name")
            context["statement_expectation"] = _read_statement_expectation(frame, frame_kind["kind"])
            if not context["sql_text"]:
                context["sql_text"] = _read_cpp_string(frame, "this->base_sql_query")
            context["running_loops"] = _read_running_loops(frame, "context.running_loops")
        else:
            context["file_name"] = _read_cpp_string(frame, "file_name")
            context["query_line"] = _read_int(frame, "query_line")
            context["sql_text"] = _read_cpp_string(frame, "context.sql_query")
            context["connection_name"] = _read_cpp_string(frame, "this->connection_name")
            context["running_loops"] = _read_running_loops(frame, "context.running_loops")

        context["loop_values"] = {loop["name"]: loop["value"] for loop in context["running_loops"]}

        if best_match is None or context["priority"] < best_match["priority"]:
            best_match = context

    return best_match


def _classify_frame(function_name):
    if "duckdb::Query::ExecuteInternal" in function_name:
        return {"priority": 0, "kind": "query", "style": "execute_internal"}
    if "duckdb::Statement::ExecuteInternal" in function_name:
        return {"priority": 0, "kind": "statement", "style": "execute_internal"}
    if "duckdb::Command::ExecuteQuery" in function_name:
        return {"priority": 1, "kind": None, "style": "execute_query"}
    return None


def _read_running_loops(frame, expression):
    loop_count = _read_int(frame, "{}.size()".format(expression))
    if loop_count is None or loop_count <= 0:
        return []

    loops = []
    for index in range(loop_count):
        loop_expr = "{}[{}]".format(expression, index)
        loop_name = _read_cpp_string(frame, "{}.loop_iterator_name".format(loop_expr))
        loop_idx = _read_int(frame, "{}.loop_idx".format(loop_expr))
        token_count = _read_int(frame, "{}.tokens.size()".format(loop_expr))

        if loop_name is None or loop_idx is None:
            continue

        if token_count is not None and token_count > 0:
            loop_value = _read_cpp_string(frame, "{}.tokens[{}]".format(loop_expr, loop_idx))
        else:
            loop_value = str(loop_idx)

        loops.append({"name": loop_name, "value": loop_value})
    return loops


def _read_statement_expectation(frame, kind):
    if kind != "statement":
        return None

    expected_result = _read_int(frame, "(int)(this->expected_result)")
    if expected_result == 0:
        return "ok"
    if expected_result == 1:
        return "error"
    return None


def _read_cpp_string(frame, expression):
    value = frame.EvaluateExpression("(const char *)({}).c_str()".format(expression))
    if not value.IsValid() or not value.GetError().Success():
        return None

    summary = value.GetSummary()
    if summary:
        return _strip_quotes(summary)

    raw_value = value.GetValue()
    if raw_value:
        return raw_value
    return None


def _read_int(frame, expression):
    value = frame.EvaluateExpression("(long long)({})".format(expression))
    if not value.IsValid() or not value.GetError().Success():
        return None
    return value.GetValueAsSigned()


def _strip_quotes(text):
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        try:
            return ast.literal_eval(text)
        except (SyntaxError, ValueError):
            return text[1:-1]
    return text


def _format_statement_context(context):
    lines = []
    file_name = context["file_name"] or "<unknown file>"
    query_line = context["query_line"]
    kind = _format_kind(context)

    first_line = file_name
    if query_line is not None:
        first_line = "{}:{}".format(file_name, query_line)
    if kind:
        first_line = "{} [{}]".format(first_line, kind)
    if context["connection_name"]:
        first_line = "{} connection={}".format(first_line, context["connection_name"])
    lines.append(first_line)

    if context["running_loops"]:
        loop_text = ", ".join("{}={}".format(loop["name"], loop["value"]) for loop in context["running_loops"])
        lines.append("loops: {}".format(loop_text))

    sql_text = context["sql_text"]
    if sql_text:
        lines.append(sql_text)
    return "\n".join(lines)


def _matches_context(context, matcher):
    if matcher["file"] is not None:
        if context["file_name"] is None or matcher["file"] not in context["file_name"]:
            return False

    query_line = context["query_line"]
    if matcher["line"] is not None and query_line != matcher["line"]:
        return False
    if matcher["line_min"] is not None:
        if query_line is None or query_line < matcher["line_min"]:
            return False
    if matcher["line_max"] is not None:
        if query_line is None or query_line > matcher["line_max"]:
            return False

    if matcher["kind"] is not None:
        if matcher["kind"] == "query":
            if context["kind"] != "query":
                return False
        elif matcher["kind"] == "statement":
            if context["kind"] != "statement":
                return False
        elif matcher["kind"] == "statement_ok":
            if context["kind"] != "statement" or context["statement_expectation"] != "ok":
                return False
        elif matcher["kind"] == "statement_error":
            if context["kind"] != "statement" or context["statement_expectation"] != "error":
                return False
    if matcher["connection"] is not None and context["connection_name"] != matcher["connection"]:
        return False
    if matcher["sql"] is not None:
        if context["sql_text"] is None or matcher["sql"] not in context["sql_text"]:
            return False

    for loop_filter in matcher["loops"]:
        loop_name = loop_filter["name"]
        if loop_name not in context["loop_values"]:
            return False
        loop_value = loop_filter["value"]
        if loop_value is not None and context["loop_values"][loop_name] != loop_value:
            return False

    return True


def _describe_matcher(matcher):
    parts = []
    if matcher["file"] is not None:
        parts.append("file contains {!r}".format(matcher["file"]))
    if matcher["line"] is not None:
        parts.append("line={}".format(matcher["line"]))
    else:
        if matcher["line_min"] is not None:
            parts.append("line>={}".format(matcher["line_min"]))
        if matcher["line_max"] is not None:
            parts.append("line<={}".format(matcher["line_max"]))
    if matcher["kind"] is not None:
        parts.append("kind={}".format(matcher["kind"]))
    if matcher["connection"] is not None:
        parts.append("connection={}".format(matcher["connection"]))
    if matcher["sql"] is not None:
        parts.append("SQL contains {!r}".format(matcher["sql"]))
    for loop_filter in matcher["loops"]:
        if loop_filter["value"] is None:
            parts.append("loop {}".format(loop_filter["name"]))
        else:
            parts.append("loop {}={}".format(loop_filter["name"], loop_filter["value"]))
    if not parts:
        return "match any sqllogictest statement"
    return ", ".join(parts)


def _format_kind(context):
    if context["kind"] != "statement":
        return context["kind"]

    expectation = context["statement_expectation"]
    if expectation == "ok":
        return "statement ok"
    if expectation == "error":
        return "statement error"
    return "statement"


def _iter_breakpoints(target):
    for index in range(target.GetNumBreakpoints()):
        yield target.GetBreakpointAtIndex(index)


def _restore_next_state(target):
    global _NEXT_STATE
    if not _NEXT_STATE:
        return

    for breakpoint_id, auto_continue in _NEXT_STATE["saved_auto_continue"].items():
        breakpoint = target.FindBreakpointByID(breakpoint_id)
        if breakpoint.IsValid():
            breakpoint.SetAutoContinue(auto_continue)

    helper_breakpoint_id = _NEXT_STATE["helper_breakpoint_id"]
    helper_breakpoint = target.FindBreakpointByID(helper_breakpoint_id)
    if helper_breakpoint.IsValid():
        target.BreakpointDelete(helper_breakpoint_id)

    _NEXT_STATE = None


def _restore_next_watch_state(target):
    global _NEXT_WATCH_STATE
    if not _NEXT_WATCH_STATE:
        return

    for breakpoint_id, auto_continue in _NEXT_WATCH_STATE["saved_auto_continue"].items():
        breakpoint = target.FindBreakpointByID(breakpoint_id)
        if breakpoint.IsValid():
            breakpoint.SetAutoContinue(auto_continue)

    for breakpoint_id, auto_continue in _NEXT_WATCH_STATE["saved_watch_auto_continue"].items():
        breakpoint = target.FindBreakpointByID(breakpoint_id)
        if breakpoint.IsValid():
            breakpoint.SetAutoContinue(auto_continue)

    _NEXT_WATCH_STATE = None


def _cleanup_dead_watches(target):
    dead_breakpoints = []
    for breakpoint_id in _WATCHES:
        breakpoint = target.FindBreakpointByID(breakpoint_id)
        if not breakpoint.IsValid():
            dead_breakpoints.append(breakpoint_id)

    for breakpoint_id in dead_breakpoints:
        del _WATCHES[breakpoint_id]


def _next_matching_statement_callback(frame, breakpoint_location, _internal_dict):
    breakpoint_id = breakpoint_location.GetBreakpoint().GetID()
    target = frame.GetThread().GetProcess().GetTarget()

    if not _NEXT_STATE or _NEXT_STATE["helper_breakpoint_id"] != breakpoint_id:
        return False

    context = _find_sqllogictest_context(frame.GetThread().GetProcess().GetThreadAtIndex(0))
    if context is None:
        _restore_next_state(target)
        print("sql_next_matching_statement: failed to extract sqllogictest context")
        return True

    if _is_duplicate_statement_hook(frame, context):
        return False
    if not _matches_context(context, _NEXT_STATE["matcher"]):
        return False

    _restore_next_state(target)
    print(_format_statement_context(context))
    return True


def _watch_statement_callback(frame, breakpoint_location, _internal_dict):
    if _NEXT_STATE:
        return False

    breakpoint_id = breakpoint_location.GetBreakpoint().GetID()
    watch = _WATCHES.get(breakpoint_id)
    if watch is None:
        return True

    if not _NEXT_WATCH_STATE:
        return False
    if breakpoint_id not in _NEXT_WATCH_STATE["selected_watch_ids"]:
        return False

    context = _find_sqllogictest_context(frame.GetThread().GetProcess().GetThreadAtIndex(0))
    if context is None:
        print("sql_watch_statement: failed to extract sqllogictest context")
        return True

    if _is_duplicate_statement_hook(frame, context):
        return False
    if not _matches_context(context, watch["matcher"]):
        return False

    target = frame.GetThread().GetProcess().GetTarget()
    if _NEXT_WATCH_STATE:
        _restore_next_watch_state(target)

    print(_format_statement_context(context))
    return True


def _is_duplicate_statement_hook(frame, context):
    # Statements hit query_break both before and inside Command::ExecuteQuery.
    caller = frame.GetThread().GetFrameAtIndex(frame.GetFrameID() + 1)
    return context["kind"] == "statement" and "duckdb::Command::ExecuteQuery" in (caller.GetFunctionName() or "")
