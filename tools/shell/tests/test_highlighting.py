# fmt: off

import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest
import os

lineitem_ddl = 'CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);'

@pytest.mark.skipif(os.name == 'nt', reason="Windows highlighting does not use shell escapes")
def test_highlight_column_header(shell):
    test = (
        ShellTest(shell)
        .statement(".highlight_results on")
        .statement('select NULL AS r;')
    )
    result = test.run()
    result.check_stdout('\x1b[90mNULL\x1b[00m')
@pytest.mark.skipif(os.name == 'nt', reason="Windows highlighting does not use shell escapes")
def test_custom_highlight(shell):
    test = (
        ShellTest(shell)
        .statement(".highlight_results on")
        .statement(".highlight_colors column_name red bold")
        .statement(".highlight_colors column_type yellow")
        .statement(lineitem_ddl)
        .statement('select * from lineitem;')
    )
    result = test.run()
    result.check_stdout('\x1b[1m\x1b[31ml_comment\x1b[00m')
    result.check_stdout('\x1b[33mvarchar\x1b[00m')

def test_custom_highlight_error(shell):
    test = (
        ShellTest(shell)
        .statement(".highlight_colors column_nameXX red")
        .statement(".highlight_colors column_name redXX")
        .statement(".highlight_colors column_name red boldXX")
        .statement(".highlight_colors column_name red bold zz")
    )
    result = test.run()
    result.check_stderr("Unknown element 'column_nameXX'")
    result.check_stderr("Unknown highlighting color 'redXX'")
    result.check_stderr("Unknown intensity 'boldXX'")
    result.check_stderr("Usage")

@pytest.mark.skipif(os.name == 'nt', reason="Deprecated highlighting commands")
def test_deprecated_highlight_commands(shell):
    test = (
        ShellTest(shell)
        .statement(".keyword brightred")
        .statement(".comment brightred")
        .statement(".error brightred")
        .statement(".cont brightred")
        .statement(".cont_sel brightred")
        .statement("select 42;")
    )
    result = test.run()
    result.check_stdout("42")
    result.check_stderr("use .highlight_colors keyword brightred instead")
    result.check_stderr("use .highlight_colors comment brightred instead")
    result.check_stderr("use .highlight_colors error brightred instead")
    result.check_stderr("use .highlight_colors continuation brightred instead")
    result.check_stderr("use .highlight_colors continuation_selected brightred instead")
    assert "Unknown" not in result.stderr
    assert "render_color" not in result.stderr

@pytest.mark.skipif(os.name == 'nt', reason="Deprecated highlighting commands")
def test_deprecated_highlight_constant(shell):
    test = ShellTest(shell).statement(".constant brightred")
    result = test.run()
    result.check_stderr(".constant has been split into numeric_constant and string_constant")
    result.check_stderr(".highlight_colors numeric_constant brightred and .highlight_colors string_constant brightred")

def run_in_pty(shell, arguments, term, response=None, no_color=None, extra_env=None):
    # runs the shell on a pseudo-terminal that either stays silent or answers the background color query
    import pty, select, time
    master, slave = pty.openpty()
    env = ShellTest(shell).get_environment()
    env.pop('NO_COLOR', None)
    env.pop('TERM', None)
    env.pop('CI', None)
    env.pop('COLORFGBG', None)
    env.update(extra_env or {})
    if no_color is not None:
        env['NO_COLOR'] = no_color
    if term is not None:
        env['TERM'] = term
    start = time.monotonic()
    process = subprocess.Popen([shell, '-no-init', *arguments], stdin=slave, stdout=slave, stderr=slave, env=env)
    os.close(slave)
    output = b''
    answered = False
    try:
        while process.poll() is None and time.monotonic() - start < 20:
            if select.select([master], [], [], 0.05)[0]:
                try:
                    output += os.read(master, 4096)
                except OSError:
                    break
            if response and not answered and b'\x1b]11;?' in output:
                os.write(master, response)
                answered = True
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
    return process.returncode, output, time.monotonic() - start

# a headless pseudo-terminal (CI runner, docker run -t) never answers the background color query
@pytest.mark.skipif(os.name == 'nt', reason="requires a POSIX pseudo-terminal")
@pytest.mark.parametrize("term, arguments", [
    ('xterm-256color', ['--version']),
    ('xterm-256color', ['-json', '-c', 'SELECT 42']),
    ('xterm-256color', ['-csv', '-c', 'SELECT 42']),
    ('xterm-256color', ['-agent', '-c', 'SELECT 42']),
    ('xterm-256color', ['-dark-mode', '-manual', 'list_contains']),
    ('xterm-256color', ['-light-mode', '-c', 'SELECT 42']),
    ('dumb', ['-c', 'SELECT 42']),
    ('dumb', ['-help']),
    (None, ['-c', 'SELECT 42']),
    (None, ['-help']),
])
def test_no_color_probe_when_not_needed(shell, term, arguments):
    status, output, elapsed = run_in_pty(shell, arguments, term)
    assert status == 0, output
    assert b'\x1b]11;?' not in output
    assert b'Timeout trying to read terminal background color' not in output
    assert elapsed < 4

@pytest.mark.skipif(os.name == 'nt', reason="requires a POSIX pseudo-terminal")
@pytest.mark.parametrize("arguments", [
    ['-c', 'SELECT 42'],
    ['-c', 'SELECT missing_column'],
    ['-help'],
    ['-manual', 'list_contains'],
])
def test_no_color_environment_variable(shell, arguments):
    import re
    status, output, elapsed = run_in_pty(shell, arguments, 'xterm-256color', no_color='1')
    assert status in (0, 1), output
    assert b'\x1b]11;?' not in output
    assert not re.search(rb'\x1b\[[0-9;]*m', output)
    assert elapsed < 4

@pytest.mark.skipif(os.name == 'nt', reason="requires a POSIX pseudo-terminal")
def test_no_color_probe_in_ci(shell):
    status, output, elapsed = run_in_pty(shell, ['-c', 'SELECT missing_column'], 'xterm-256color', extra_env={'CI': 'true'})
    assert status != 0, output
    assert b'\x1b]11;?' not in output
    assert b'\x1b[' in output  # still colored
    assert elapsed < 4

@pytest.mark.skipif(os.name == 'nt', reason="requires a POSIX pseudo-terminal")
@pytest.mark.parametrize("colorfgbg, error_color", [
    ('0;15', b'\x1b[38;5;124m'),  # light background
    ('15;0', b'\x1b[38;5;203m'),  # dark background
    ('15;default;0', b'\x1b[38;5;203m'),
])
def test_colorfgbg(shell, colorfgbg, error_color):
    status, output, elapsed = run_in_pty(shell, ['-c', 'SELECT missing_column'], 'xterm-256color', extra_env={'COLORFGBG': colorfgbg})
    assert status != 0, output
    assert b'\x1b]11;?' not in output
    assert error_color in output
    assert elapsed < 4

@pytest.mark.skipif(os.name == 'nt', reason="requires a POSIX pseudo-terminal")
def test_color_probe_answered_by_terminal(shell):
    response = b'\x1b]11;rgb:ffff/ffff/ffff\x07\x1b[?62c'
    status, output, _ = run_in_pty(shell, ['-c', 'SELECT missing_column'], 'xterm-256color', response)
    assert status != 0, output
    assert b'\x1b]11;?' in output
    assert b'\x1b[38;5;124m' in output  # light-mode error color

@pytest.mark.skipif(os.name == 'nt', reason="requires a POSIX pseudo-terminal")
def test_manual_uses_detected_color_mode(shell):
    response = b'\x1b]11;rgb:ffff/ffff/ffff\x07\x1b[?62c'
    status, output, _ = run_in_pty(shell, ['-manual', 'list_contains'], 'xterm-256color', response)
    assert status == 0, output
    assert output.count(b'\x1b]11;?') == 1

# fmt: on
