#!/usr/bin/env python3

import argparse
import re
from pathlib import Path


ERROR_PATTERN = re.compile(r"^[^\s].*: error:")
CLANG_TIDY_INVOCATION_PATTERN = re.compile(r"^(?:\S*/)?clang-tidy(?:-\d+)?(?:\s|$)")
CLANG_TIDY_STATUS_PATTERNS = (
    re.compile(r"^\d+ warnings?(?: and \d+ errors?)? generated\.$"),
    re.compile(r"^Suppressed \d+ warnings?.*$"),
    re.compile(r"^Error while processing .*$"),
    re.compile(r"^Found compiler error\(s\)\.$"),
)
BUILD_STATUS_PATTERN = re.compile(r"^(?:make(?:\[\d+\])?:|ninja:)")
FAILURE_TAIL_LINES = 100


def is_error(line):
    return ERROR_PATTERN.search(line) is not None


def is_diagnostic_boundary(line):
    if CLANG_TIDY_INVOCATION_PATTERN.search(line) or BUILD_STATUS_PATTERN.search(line):
        return True
    return any(pattern.search(line) for pattern in CLANG_TIDY_STATUS_PATTERNS)


def unique_error_blocks(lines):
    blocks = {}
    current_error = None
    current_block = []

    def finish_block():
        if current_error is None:
            return
        while len(current_block) > 1 and not current_block[-1]:
            current_block.pop()
        if current_error not in blocks:
            blocks[current_error] = list(current_block)

    for line in lines:
        if is_error(line):
            finish_block()
            current_error = line
            current_block = [line]
        elif current_error is not None:
            if is_diagnostic_boundary(line):
                finish_block()
                current_error = None
                current_block = []
            else:
                current_block.append(line)

    finish_block()
    return [blocks[error] for error in sorted(blocks)]


def summarize(lines, command_exit_code):
    blocks = unique_error_blocks(lines)
    if blocks:
        label = "error" if len(blocks) == 1 else "errors"
        output = [f"Found {len(blocks)} unique clang-tidy {label}:", ""]
        for block_index, block in enumerate(blocks):
            if block_index:
                output.append("")
            output.extend(block)
        return output

    if command_exit_code:
        return [
            "Clang-tidy failed without a recognized diagnostic; showing the final "
            f"{min(FAILURE_TAIL_LINES, len(lines))} log lines:",
            "",
            *lines[-FAILURE_TAIL_LINES:],
        ]

    return ["No clang-tidy errors found."]


def main():
    parser = argparse.ArgumentParser(description="Print a deduplicated summary of a clang-tidy log")
    parser.add_argument("log_file", type=Path, help="complete clang-tidy log")
    parser.add_argument("--command-exit-code", type=int, required=True, help="exit code from the clang-tidy command")
    args = parser.parse_args()

    lines = args.log_file.read_text(encoding="utf-8", errors="replace").splitlines()
    print("\n".join(summarize(lines, args.command_exit_code)))


if __name__ == "__main__":
    main()
