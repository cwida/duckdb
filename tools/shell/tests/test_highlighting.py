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

# fmt: on
