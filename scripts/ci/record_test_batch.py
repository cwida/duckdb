"""Run a test batch and preserve its input and output on failure."""

import argparse
import json
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command and command[0] == "--":
        command = command[1:]
    if not command or "-f" not in command:
        parser.error("Expected a test command with -f <batch file>")

    result = subprocess.run(command, capture_output=True)
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    if result.returncode:
        batch_index = command.index("-f") + 1
        batch = Path(command[batch_index])
        directory = arguments.output / batch.stem
        directory.mkdir(parents=True, exist_ok=True)
        saved_batch = directory / "tests.txt"
        saved_batch.write_bytes(batch.read_bytes())
        (directory / "stdout.log").write_bytes(result.stdout)
        (directory / "stderr.log").write_bytes(result.stderr)
        command[batch_index] = str(saved_batch)
        (directory / "command.json").write_text(
            json.dumps({"command": command, "returncode": result.returncode}, indent=2) + "\n", encoding="utf-8"
        )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
