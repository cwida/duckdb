# fmt: off

import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest
import os
from pathlib import Path


# test import from a parquet file
def test_import_parquet(shell):
    test = (
        ShellTest(shell)
        .statement(f'.import data/parquet-testing/unsigned.parquet a')
        .statement("SELECT MAX(d) FROM a")
    )

    result = test.run()
    result.check_stdout("18446744073709551615")

# test import from a json file
def test_import_json(shell):
    test = (
        ShellTest(shell)
        .statement(f'.import data/json/example_n.ndjson a')
        .statement("SELECT name FROM a")
    )

    result = test.run()
    result.check_stdout("Broadcast News")

# importing into a temporary view's name inside an open transaction creates a table and keeps the transaction
def test_import_into_temporary_view_name_keeps_transaction(shell):
    test = (
        ShellTest(shell)
        .statement("CREATE TABLE u (k INTEGER)")
        .statement("CREATE TEMPORARY VIEW tv AS SELECT 1 AS k")
        .statement("BEGIN")
        .statement("INSERT INTO u VALUES (1)")
        .statement('.import data/json/example_n.ndjson tv')
        .statement("INSERT INTO u VALUES (2)")
        .statement("COMMIT")
        .statement("SELECT 'writes=' || count(*) FROM u")
        .statement("SELECT name FROM memory.main.tv")
    )

    result = test.run()
    result.check_stdout("writes=2")
    result.check_stdout("Broadcast News")


# fmt: on
