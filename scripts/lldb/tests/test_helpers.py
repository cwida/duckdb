"""Integration tests loaded by run_tests.py inside LLDB."""

import importlib
import contextlib
import io
import json
from pathlib import Path
import shlex
import sys
import trace
import unittest

try:
    import lldb
except ImportError:
    lldb = None


SCRIPTS = Path(__file__).resolve().parents[1]
HELPERS = (
    "sqllogictest_breakpoints/sql_break.py",
    "pointer_print/pointer_print.py",
    "print_array/print_array.py",
    "filter_boundschecks/filter_checks.py",
)
DEBUGGER = None
EXECUTABLE = None
VALUES = None


@unittest.skipIf(lldb is None, "run with run_tests.py inside LLDB")
class HelperTests(unittest.TestCase):
    def command(self, command, success=True):
        result = lldb.SBCommandReturnObject()
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            DEBUGGER.GetCommandInterpreter().HandleCommand(command, result)
        output = (result.GetOutput() or "") + (result.GetError() or "") + errors.getvalue()
        self.assertNotIn("Traceback", output, command + "\n" + output)
        self.assertEqual(result.Succeeded(), success, command + "\n" + output)
        return output

    def tearDown(self):
        for index in reversed(range(DEBUGGER.GetNumTargets())):
            target = DEBUGGER.GetTargetAtIndex(index)
            process = target.GetProcess()
            if process.IsValid() and process.GetState() not in (lldb.eStateExited, lldb.eStateDetached):
                process.Kill()
            DEBUGGER.DeleteTarget(target)
        sql = importlib.import_module("sql_break")
        sql._WATCHES.clear()
        sql._NEXT_STATE = None
        sql._NEXT_WATCH_STATE = None

    def start(self, executable, breakpoint, arguments=""):
        self.command("target create " + shlex.quote(executable))
        if arguments:
            self.command("settings set -- target.run-args " + arguments)
        self.command(breakpoint)
        self.command("run")
        target = DEBUGGER.GetSelectedTarget()
        self.assertEqual(target.GetProcess().GetState(), lldb.eStateStopped)
        return target

    def start_values(self, marker="LLDB_TEST_STOP"):
        source = SCRIPTS / "tests/values.cpp"
        line = next(index for index, text in enumerate(source.read_text().splitlines(), 1) if marker in text)
        return self.start(VALUES, "breakpoint set --file values.cpp --line {}".format(line))

    def start_sql(self):
        return self.start(
            EXECUTABLE,
            "breakpoint set --name query_break --one-shot true",
            "--test-dir {} test/statements.test".format(shlex.quote(str(SCRIPTS / "tests"))),
        )

    def current(self):
        return self.command("sql_current_statement")

    def test_no_target_and_help(self):
        for command in ("sql_current_statement", "sql_next_statement", "sql_watch_statement", "sql_next_watch"):
            with self.subTest(command=command):
                self.assertIn("no selected process", self.command(command, success=False))
        self.assertIn("no selected target", self.command("print_array data 1", success=False))
        self.assertIn("usage:", self.command("print_array --help"))

    def test_malformed_arguments(self):
        for command in (
            'print_array "',
            'sql_next_matching_statement --file "',
            'sql_watch_statement --loop "',
            'sql_next_watch "',
            'sql_delete_watch "',
            "print_array",
            "sql_current_statement unexpected",
            "sql_next_statement unexpected",
            "sql_next_matching_statement --kind invalid",
            "sql_next_matching_statement --line 1 --line-min 1",
            "sql_next_matching_statement --line-min 5 --line-max 1",
            "sql_watch_statement --loop =1",
            "sql_watch_statement --loop i=",
            "sql_next_watch not-an-id",
        ):
            with self.subTest(command=command):
                self.command(command, success=False)

    def test_array_values_and_expressions(self):
        self.start_values()
        output = self.command("print_array data_ptr count")
        for value in (11, 22, 33, 44):
            self.assertIn(str(value), output)
        output = self.command('print_array "data_ptr + 1" "count - 2"')
        self.assertIn("22", output)
        self.assertIn("33", output)
        self.assertNotIn("44", output)

    def test_array_invalid_values(self):
        self.start_values()
        for command in (
            "print_array data_ptr 0",
            "print_array data_ptr -1",
            "print_array data_ptr 1.5",
            "print_array data_ptr missing_count",
            "print_array count 1",
            'print_array "(int64_t *)0" 1',
            "print_array missing_pointer 1",
        ):
            with self.subTest(command=command):
                self.command(command, success=False)

    def test_pointer_output_and_errors_are_captured(self):
        self.start_values()
        for name in ("optional", "owned", "shared"):
            with self.subTest(name=name):
                output = self.command("duckdb-p " + name)
                self.assertIn("DebugValue", output)
                self.assertIn("number = 42", output)
        self.assertIn("nullptr", self.command("p null_optional"))
        self.assertIn("0x", self.command("duckdb-p /x count"))
        self.command("duckdb-p missing_variable", success=False)

    def test_print_evaluates_expression_once(self):
        self.start_values()
        self.command("duckdb-p ++evaluations")
        value = (
            DEBUGGER.GetSelectedTarget().GetProcess().GetSelectedThread().GetSelectedFrame().FindVariable("evaluations")
        )
        self.assertEqual(value.GetValueAsSigned(), 1)

    def test_array_size_is_evaluated_once(self):
        self.start_values()
        self.assertIn("11", self.command("print_array data_ptr ++evaluations"))
        value = (
            DEBUGGER.GetSelectedTarget().GetProcess().GetSelectedThread().GetSelectedFrame().FindVariable("evaluations")
        )
        self.assertEqual(value.GetValueAsSigned(), 1)

    def test_step_avoid_skips_wrapper(self):
        target = self.start_values("LLDB_STEP_START")
        self.command("duckdb-step-avoid-enable")
        self.command("thread step-in")
        self.assertEqual(target.GetProcess().GetSelectedThread().GetSelectedFrame().GetFunctionName(), "main")

    def test_step_avoid_can_enter_wrapper(self):
        target = self.start_values("LLDB_STEP_START")
        self.command("duckdb-step-avoid-disable")
        self.command("thread step-in")
        self.assertIn("::get()", target.GetProcess().GetSelectedThread().GetSelectedFrame().GetFunctionName())

    def test_step_avoid_restores_setting(self):
        self.command("duckdb-step-avoid-disable")
        self.command("settings set target.process.thread.step-avoid-regexp '^custom::'")
        self.command("duckdb-step-avoid-enable")
        first = self.command("duckdb-step-avoid-show")
        self.assertIn("^custom::", first)
        self.assertIn("duckdb::vector", first)
        self.command("duckdb-step-avoid-enable")
        self.assertEqual(first, self.command("duckdb-step-avoid-show"))
        self.command("duckdb-step-avoid-disable")
        self.assertIn("'^custom::'", self.command("duckdb-step-avoid-show"))
        self.command("duckdb-step-avoid-disable")

    def test_next_statement_advances_past_duplicate_hooks(self):
        self.start_sql()
        self.assertIn("CREATE TABLE", self.current())
        self.command("sql_next_statement")
        self.assertIn("INSERT INTO", self.current())

    def test_sql_filters_and_breakpoint_restoration(self):
        target = self.start_sql()
        regular = target.BreakpointCreateByName("query_break")
        disabled = target.BreakpointCreateByName("query_break")
        disabled.SetEnabled(False)
        automatic = target.BreakpointCreateByName("query_break")
        automatic.SetAutoContinue(True)
        self.command("sql_next_matching_statement --kind statement_error")
        self.assertIn("missing_column", self.current())
        self.assertFalse(regular.GetAutoContinue())
        self.assertFalse(disabled.IsEnabled())
        self.assertTrue(automatic.GetAutoContinue())
        self.command("sql_next_matching_statement --connection con2 --loop i=1 --kind query")
        self.assertIn("i=1", self.current())
        self.assertIn("SELECT 1 - 1 AS loop_value", self.current())
        self.command("sql_next_matching_statement --loop i=2")
        self.assertIn("i=2", self.current())

    def test_json_context_and_sql_filter(self):
        self.start_sql()
        context = json.loads(self.command("sql_current_statement --json"))
        self.assertEqual(context["file_name"], "test/statements.test")
        self.assertEqual(context["query_line"], 5)
        self.assertEqual(context["kind"], "statement")
        self.assertEqual(context["statement_expectation"], "ok")
        self.assertEqual(context["running_loops"], [])
        self.command('sql_next_matching_statement --sql "AS loop_value" --connection con2 --loop i=2')
        context = json.loads(self.command("sql_current_statement --json"))
        self.assertEqual(context["sql_text"], "SELECT 2 - 2 AS loop_value")
        self.assertEqual(context["loop_values"], {"i": "2"})
        self.assertEqual(context["connection_name"], "con2")
        self.command('sql_next_matching_statement --sql final_value')
        context = json.loads(self.command("sql_current_statement --json"))
        self.assertEqual(context["sql_text"], """SELECT 'quoted "value"' AS final_value""")

    def test_search_without_match_restores_breakpoints(self):
        target = self.start_sql()
        regular = target.BreakpointCreateByName("query_break")
        self.command('sql_next_matching_statement --sql "not in the fixture"')
        self.assertEqual(target.GetProcess().GetState(), lldb.eStateExited)
        self.assertEqual(target.GetProcess().GetExitStatus(), 0)
        self.assertFalse(regular.GetAutoContinue())
        self.assertEqual(target.GetNumBreakpoints(), 1)

    def test_watch_lifecycle_and_ordinary_continue(self):
        target = self.start_sql()
        self.command("sql_watch_statement --kind query --loop i")
        sql = importlib.import_module("sql_break")
        watch_id = next(iter(sql._WATCHES))
        self.assertIn(str(watch_id), self.command("sql_list_watches"))
        self.command("sql_next_watch {}".format(watch_id))
        self.assertIn("i=0", self.current())
        self.assertTrue(target.FindBreakpointByID(watch_id).GetAutoContinue())
        self.command("sql_next_watch")
        self.assertIn("i=1", self.current())
        self.command("sql_watch_statement --kind statement_error")
        self.command("sql_delete_watch")
        self.assertEqual(len(sql._WATCHES), 1)
        self.command("sql_delete_watch 999999", success=False)
        self.command("sql_next_watch 999999", success=False)
        self.command("continue")
        self.assertEqual(target.GetProcess().GetState(), lldb.eStateExited)
        self.command("sql_delete_watch all")
        self.assertIn("No sql watches", self.command("sql_list_watches"))

    def test_statement_watch_skips_duplicate_hooks(self):
        self.start_sql()
        self.command('sql_watch_statement --kind statement_ok --sql "lldb_values"')
        self.command("sql_next_watch")
        self.assertIn("INSERT INTO", self.current())
        self.assertIn("SQL contains", self.command("sql_list_watches"))
        self.command("sql_delete_watch all")


def run(debugger, executable, values, report, coverage_dir=None):
    global DEBUGGER, EXECUTABLE, VALUES
    DEBUGGER, EXECUTABLE, VALUES = debugger, executable, values
    debugger.SetAsync(False)

    def run_suite():
        for helper in HELPERS:
            result = lldb.SBCommandReturnObject()
            debugger.GetCommandInterpreter().HandleCommand(
                "command script import " + shlex.quote(str(SCRIPTS / helper)), result
            )
            if not result.Succeeded():
                raise RuntimeError(result.GetError())
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(HelperTests)
        return unittest.TextTestRunner(stream=output, verbosity=2).run(suite)

    output = io.StringIO()
    if coverage_dir:
        tracer = trace.Trace(count=True, trace=False, ignoredirs=[sys.base_prefix])
        result = tracer.runfunc(run_suite)
        coverage = tracer.results()
        sources = {str(SCRIPTS / helper) for helper in HELPERS}
        coverage.counts = {key: count for key, count in coverage.counts.items() if key[0] in sources}
        coverage.write_results(show_missing=True, summary=True, coverdir=coverage_dir)
    else:
        result = run_suite()
    Path(report).write_text(
        json.dumps({"successful": result.wasSuccessful(), "tests": result.testsRun, "output": output.getvalue()})
    )
