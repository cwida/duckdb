import os
import subprocess
from typing import List, Union
import sys

import pytest


def pytest_addoption(parser):
    parser.addoption(
        "--shell-binary",
        action="store",
        default=None,
        help="Provide the shell binary to use for the tests",
    )
    parser.addoption("--start-offset", action="store", type=int, help="Skip the first 'n' tests")


def pytest_collection_modifyitems(config, items):
    start_offset = config.getoption("--start-offset")
    if not start_offset:
        # --skiplist not given in cli, therefore move on
        return

    skipped = pytest.mark.skip(reason="included in --skiplist")
    skipped_items = items[:start_offset]
    for item in skipped_items:
        item.add_marker(skipped)


class TestResult:
    def __init__(self, stdout, stderr, status_code):
        self.stdout: Union[str, bytes] = stdout
        self.stderr: Union[str, bytes] = stderr
        self.status_code: int = status_code

    def check_stdout(self, expected: Union[str, List[str], bytes, None]):
        if expected is None:
            assert self.stdout == ""
            return
        if isinstance(expected, list):
            expected = "\n".join(expected)
        if self.status_code != 0:
            print(self.stderr, file=sys.stderr)
        assert self.status_code == 0
        if not is_needle_in_haystack(expected, self.stdout):
            print(self.stdout, file=sys.stderr)
            assert is_needle_in_haystack(expected, self.stdout)

    def check_not_exist(self, not_exist: Union[str, List[str], bytes]):
        if isinstance(not_exist, list):
            not_exist = "\n".join(not_exist)
        assert self.status_code == 0
        if is_needle_in_haystack(not_exist, self.stdout):
            print(self.stdout, file=sys.stderr)
            assert not is_needle_in_haystack(not_exist, self.stdout)

    def check_stderr(self, expected):
        if expected is None:
            assert self.stderr == ""
        else:
            assert is_needle_in_haystack(expected, self.stderr)


class ShellTest:
    def __init__(self, shell, arguments=None):
        if not shell:
            raise ValueError("Please provide a shell binary")
        arguments = list(arguments or [])
        self.shell = shell
        self.arguments = [shell, "--batch"] + arguments
        if "-init" not in arguments and "--init" not in arguments:
            self.arguments.append("--no-init")
        self.statements: List[str] = []
        self.input = None
        self.output = None
        self.environment = {}

    def add_argument(self, *args):
        self.arguments.extend(args)
        return self

    def statement(self, stmt):
        self.statements.append(stmt)
        return self

    def env_var(self, key: str, value: str):
        self.environment[key] = value
        return self

    def query(self, *stmts):
        self.statements.extend(stmts)
        return self

    def input_file(self, file_path):
        self.input = file_path
        return self

    def output_file(self, file_path):
        self.output = file_path
        return self

    # Test Running methods

    def get_command(self, cmd: str) -> List[str]:
        command = self.arguments
        if self.input:
            command += [cmd]
        return command

    def get_input_data(self, cmd: str):
        if self.input:
            with open(self.input, "rb") as f:
                input_data = f.read()
        else:
            input_data = bytearray(cmd, "utf8")
        return input_data

    def get_statements(self):
        statements = []
        for statement in self.statements:
            if statement.startswith("."):
                statements.append(statement)
            else:
                statements.append(statement + ";")
        return "\n".join(statements)

    def get_output_data(self, res):
        if self.output:
            with open(self.output, "r") as f:
                stdout = f.read()
        else:
            stdout = res.stdout.decode("utf8").strip()
        stderr = res.stderr.decode("utf8").strip()
        return stdout, stderr

    # environment variables that switch the shell into agent mode - stripped so the tests behave the same when
    # they are run by a coding agent, tests opt in explicitly through env_var
    AGENT_ENVIRONMENT_VARIABLES = [
        "AI_AGENT",
        "AGENT",
        "CLAUDECODE",
        "CODEX_CI",
        "CODEX_SANDBOX",
        "CODEX_THREAD_ID",
        "CURSOR_AGENT",
        "GEMINI_CLI",
        "COPILOT_AGENT",
        "COPILOT_CLI",
        "COPILOT_AGENT_SESSION_ID",
        "DUCKDB_AGENT_MODE",
    ]

    def get_environment(self):
        my_env = os.environ.copy()
        for key in self.AGENT_ENVIRONMENT_VARIABLES:
            my_env.pop(key, None)
        for key, val in self.environment.items():
            my_env[key] = val
        return my_env

    def run_raw(self, stdin_text):
        """Run the shell with raw stdin text (no statement processing)."""
        command = self.arguments
        input_data = stdin_text.encode("utf8") if isinstance(stdin_text, str) else stdin_text

        my_env = self.get_environment()

        res = subprocess.run(
            command,
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=my_env,
        )
        stdout = res.stdout.decode("utf8").strip()
        stderr = res.stderr.decode("utf8").strip()
        return TestResult(stdout, stderr, res.returncode)

    def run(self):
        statements = self.get_statements()
        command = self.get_command(statements)
        input_data = self.get_input_data(statements)

        my_env = self.get_environment()

        if self.output:
            with open(self.output, "w") as output_pipe:
                res = subprocess.run(
                    command,
                    input=input_data,
                    stdout=output_pipe,
                    stderr=subprocess.PIPE,
                    env=my_env,
                )
        else:
            res = subprocess.run(
                command,
                input=input_data,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=my_env,
            )

        stdout, stderr = self.get_output_data(res)
        return TestResult(stdout, stderr, res.returncode)


@pytest.fixture()
def shell(request):
    custom_arg = request.config.getoption("--shell-binary")
    if not custom_arg:
        raise ValueError("Please provide a shell binary path to the tester, using '--shell-binary <path_to_cli>'")
    return custom_arg


@pytest.fixture()
def random_filepath(tmp_path):
    tmp_file = tmp_path / "random_import_file"
    return tmp_file


@pytest.fixture()
def generated_file(request, random_filepath):
    param = request.param
    tmp_file = random_filepath
    with open(tmp_file, "w+") as f:
        f.write(param)
    return tmp_file


def check_load_status(shell, extension: str):
    binary = ShellTest(shell)
    binary.statement(f"select loaded from duckdb_extensions() where extension_name = '{extension}';")
    result = binary.run()
    return result.stdout


def is_needle_in_haystack(needle, haystack) -> bool:
    if needle == "" or haystack == "":
        return False
    if isinstance(haystack, str) and isinstance(needle, str):
        if needle in haystack:
            return True
        if os.name == 'nt' and '\n' in needle:
            # try with windows-style newlines
            return needle.replace('\n', '\r\n') in haystack
        return False
    elif isinstance(haystack, bytes) and isinstance(needle, bytes):
        return needle in haystack
    else:
        return False


def assert_loaded(shell, extension: str):
    # TODO: add a command line argument to fail instead of skip if the extension is not loaded
    out = check_load_status(shell, extension)
    if not is_needle_in_haystack("true", out):
        pytest.skip(reason=f"'{extension}' extension is not loaded!")
    return


@pytest.fixture()
def autocomplete_extension(shell):
    assert_loaded(shell, "autocomplete")


@pytest.fixture()
def json_extension(shell):
    assert_loaded(shell, "json")
