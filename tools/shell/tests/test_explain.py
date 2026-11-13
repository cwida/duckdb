# fmt: off

import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest
import os

def test_invalid_explain(shell):
    test = (
        ShellTest(shell)
        .statement("EXPLAIN SELECT 'any_string' IN ?;")
    )
    result = test.run()


@pytest.mark.parametrize("query", [
    "EXPLAIN SELECT 42;",
    "EXPLAIN (FORMAT JSON) SELECT 42;",
])
def test_explain_trailing_newline(shell, query):
    single = subprocess.run([shell, "--no-init", "-c", query], capture_output=True, check=True).stdout
    repeated = subprocess.run([shell, "--no-init", "-c", query + query], capture_output=True, check=True).stdout
    assert single.endswith(b"\n")
    assert not single.endswith(b"\n\n")
    assert repeated == single + single


def test_explain_renderer_settings_validated(shell):
    # an invalid value is rejected without being stored, so EXPLAIN and later SETs keep working
    test = (
        ShellTest(shell)
        .statement("SET profiling_renderer_settings = MAP {'expand_all': 'bogus'};")
        .statement("EXPLAIN SELECT 42;")
        .statement("SET profiling_renderer_settings = MAP {'expand_all': 'true'};")
        .statement("SELECT current_setting('profiling_renderer_settings');")
    )
    result = test.run()
    result.check_stderr("Could not convert string 'bogus' to BOOL")
    assert "Dummy Scan" in result.stdout
    assert "expand_all=true" in result.stdout
    assert result.stderr.count("Could not convert") == 1
