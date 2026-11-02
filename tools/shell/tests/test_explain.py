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
