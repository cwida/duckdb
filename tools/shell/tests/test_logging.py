# fmt: off

import pytest
import subprocess
import sys
import os
from typing import List
from conftest import ShellTest

def test_logging(shell):
    test = (
        ShellTest(shell)
        .statement("CALL enable_logging('QueryLog', storage='stdout')")
        .statement('SELECT 1 as a')
    )
    result = test.run()

    newline = "\r\n" if os.name == "nt" else "\n"
    result.check_stdout(f"QueryLog\tINFO\tSELECT 1 as a;{newline}┌───────┐")


def test_logging_custom_delim(shell):
    test = (
        ShellTest(shell)
        .statement("CALL enable_logging('QueryLog', storage='stdout', storage_config={'delim':','})")
        .statement('SELECT 1 as a')
    )
    result = test.run()
    newline = "\r\n" if os.name == "nt" else "\n"
    result.check_stdout(f"QueryLog,INFO,SELECT 1 as a;{newline}┌───────┐")

def test_logging_shell_pretty(shell):
    # INFO/TRACE logs render as a compact single line "LEVEL:type   <elapsed>  <message>" via
    # the shell log sink. '.highlight off' makes the output deterministic (no ANSI). The elapsed
    # value is measured from CLI launch, so assert on the prefix + message, not the timing.
    test = (
        ShellTest(shell)
        .statement(".highlight off")
        .statement("CALL enable_logging('QueryLog')")
        .statement('SELECT 1 as a')
    )
    result = test.run()
    lines = result.stdout.splitlines()
    assert any(ln.startswith("INFO:QueryLog") and ln.endswith("SELECT 1 as a;") for ln in lines)


def test_logging_shell_pretty_multiline(shell):
    # A multi-line statement must be sanitized onto a single log line - no prefix-less
    # continuation lines. Regression test for the multi-line message handling.
    test = (
        ShellTest(shell)
        .statement(".highlight off")
        .statement("CALL enable_logging('QueryLog')")
        .statement("SELECT\n  1,\n  2")
    )
    result = test.run()
    lines = result.stdout.splitlines()
    assert any(ln.startswith("INFO:QueryLog") and ln.endswith("SELECT 1, 2;") for ln in lines)


def test_logging_warning_dedup(shell):
    # Repeated identical WARNINGs are de-duplicated: printed only once.
    test = (
        ShellTest(shell)
        .statement(".highlight off")
        .statement("SELECT write_log('dup', level := 'WARNING')")
        .statement("SELECT write_log('dup', level := 'WARNING')")
    )
    result = test.run()
    # The message 'dup' appears on its own line only once (result-box headers also echo the SQL,
    # so filter to standalone lines).
    lines = [ln.strip() for ln in result.stdout.splitlines()]
    assert lines.count("dup") == 1


def test_logging_error_no_dedup(shell):
    # Repeated identical ERRORs are NOT de-duplicated: every occurrence is printed.
    test = (
        ShellTest(shell)
        .statement(".highlight off")
        .statement("SELECT write_log('dup', level := 'ERROR')")
        .statement("SELECT write_log('dup', level := 'ERROR')")
    )
    result = test.run()
    lines = [ln.strip() for ln in result.stdout.splitlines()]
    assert lines.count("dup") == 2


# By default stdoutlogging has buffer size of 1, but we can increase it if we want. We use `only_flush_on_full_buffer` to ensure we can test this
def test_logging_buffering(shell):
    test = (
        ShellTest(shell)
        .statement("CALL enable_logging('QueryLog', storage='stdout', storage_buffer_size=1000, storage_config={'only_flush_on_full_buffer': true})")
        .statement('SELECT 1 as a')
        .statement('SELECT 2 as b')
    )
    result = test.run()
    result.check_not_exist("QueryLog")

# fmt: on
