# fmt: off

import os
import signal
import subprocess
import sys

import pytest

from conftest import ShellTest

resource = pytest.importorskip("resource")


def run_with_address_space_limit(shell, limit_mb, sql):
    limit = limit_mb << 20
    env = dict(os.environ)
    # agent-mode variables of the runner must not switch the shell under test into agent mode
    for key in ShellTest.AGENT_ENVIRONMENT_VARIABLES:
        env.pop(key, None)

    def limit_address_space():
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

    return subprocess.run(
        [shell, "--batch", "--no-init", ":memory:", "-c", sql],
        capture_output=True,
        env=env,
        preexec_fn=limit_address_space,
        timeout=300,
    )


# no segmentation fault or abort when the address space runs out
@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="RLIMIT_AS limits the address space on Linux")
@pytest.mark.parametrize(
    "limit_mb, sql",
    [
        (256, "SELECT 42"),
        (512, "SELECT 42"),
        (1024, "SELECT 42"),
        (2048, "SELECT 42"),
        (300, "CREATE TABLE x AS SELECT range AS i, range::VARCHAR || 'abcdefghij' AS s FROM range(20000000)"),
        (500, "CREATE TABLE x AS SELECT range AS i, range::VARCHAR || 'abcdefghij' AS s FROM range(20000000)"),
    ],
)
def test_address_space_limit(shell, limit_mb, sql):
    for _ in range(3):
        result = run_with_address_space_limit(shell, limit_mb, sql)
        assert result.returncode != -signal.SIGSEGV, result.stderr
        assert result.returncode != -signal.SIGABRT, result.stderr
        assert b"terminate called" not in result.stderr, result.stderr
        if result.returncode == 1:
            assert b"Out of Memory" in result.stderr, result.stderr
