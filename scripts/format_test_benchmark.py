#!/usr/bin/env python3
import argparse
import os
import re
import sys
from pathlib import Path

# Deprecated spellings that the sqllogictest runner still accepts with a "Replacing deprecated ..." warning
# (test/sqlite/sqllogic_test_runner.cpp, test/helpers/test_config.cpp). The formatter rewrites them so that
# `make format-check` rejects them and `make format-fix` repairs them.
DEPRECATED_KEYWORDS = {
    '__TEST_DIR__': '{TEST_DIR}',
    '__WORKING_DIRECTORY__': '{WORKING_DIRECTORY}',
    '__BUILD_DIRECTORY__': '{BUILD_DIRECTORY}',
}

# Environment variables the runner defines for every test (see the table in test/README.md).
# `${NAME}` is the deprecated spelling of `{NAME}` for these and for any variable the test declares itself.
BUILTIN_TEST_VARIABLES = {
    'TEST_NAME',
    'TEST_NAME__NO_SLASH',
    'TEST_ID',
    'TEST_UUID',
    'RUN_ID',
    'WORKING_DIR',
    'BUILD_DIR',
    'DATA_DIR',
    'LOCAL_DATA_DIR',
    'TEMP_DIR_ROOT',
    'TEMP_DIR',
    'TEMP_DIR_ABSOLUTE',
    'LOCAL_TEMP_DIR',
    'CATALOG_DIR',
}

LOOP_DIRECTIVES = {'loop', 'concurrentloop', 'foreach', 'concurrentforeach'}
DEPRECATED_VARIABLE_PATTERN = re.compile(r'\$\{([A-Za-z_][A-Za-z0-9_]*)\}')
SQLLOGICTEST_EXTENSIONS = {'.test', '.test_slow', '.test_coverage'}


def declared_test_variables(lines):
    """Collect the loop iterators and environment variables a sqllogictest file declares."""
    variables = set(BUILTIN_TEST_VARIABLES)
    for line in lines:
        tokens = line.split()
        if len(tokens) < 2:
            continue
        if tokens[0] in LOOP_DIRECTIVES:
            # `foreach a,b x,1 y,2` declares two iterators
            variables.update(name for name in tokens[1].split(',') if name)
        elif tokens[0] == 'require-env':
            variables.add(tokens[1])
        elif tokens[0] == 'require-env-not' and len(tokens) >= 3:
            # with a value list the variable must exist and is registered for substitution
            variables.add(tokens[1])
        elif tokens[0] == 'set' and tokens[1] == 'variable' and len(tokens) >= 3:
            variables.add(tokens[2])
    return variables


def replace_deprecated_syntax(full_path, lines):
    """Rewrite deprecated runner syntax to its current spelling; returns (lines, list of messages)."""
    variables = declared_test_variables(lines)
    messages = []
    result = []

    def report(line_number, old, new):
        message = f'{full_path}:{line_number}: deprecated {old} - please replace with {new}'
        if message not in messages:
            messages.append(message)

    for line_number, line in enumerate(lines, 1):
        if line.startswith('#'):
            result.append(line)
            continue
        for old, new in DEPRECATED_KEYWORDS.items():
            if old in line:
                report(line_number, old, new)
                line = line.replace(old, new)

        def replace_variable(match):
            name = match.group(1)
            if name not in variables:
                # not a loop iterator or environment variable: leave the text alone, the runner does too
                return match.group(0)
            report(line_number, '${' + name + '}', '{' + name + '}')
            return '{' + name + '}'

        line = DEPRECATED_VARIABLE_PATTERN.sub(replace_variable, line)
        result.append(line)
    return result, messages


def open_utf8(file_path, mode):
    """Open a file with UTF-8 encoding."""
    return open(file_path, mode, encoding='utf-8', errors='surrogateescape')


def format_file_content(full_path, lines):
    """Format the content of lines according to the specified logic."""
    ext = os.path.splitext(full_path)[1] if full_path != '-' else '.test'  # Assume .test for stdin
    if ext not in {'.test', '.test_slow', '.test_coverage', '.benchmark'}:
        return None, 0

    # Extract group name (use 'unknown' for stdin if no full_path, else derive from path)
    group_name = Path(full_path).parent.name if full_path != '-' else 'unknown'
    new_path_line = f'# name: {Path(full_path).as_posix()}\n'
    new_group_line = f'# group: [{group_name}]\n'

    # Find description
    found_description = False
    new_description_line = None
    for line in lines:
        if line.lower().startswith(('# description:', '#description:')):
            if found_description:
                print(
                    f"Error formatting file {full_path}, multiple lines starting with # description found",
                    file=sys.stderr,
                )
                return None, 1
            found_description = True
            new_description_line = f'# description: {line.split(":", 1)[1].strip()}\n'

    # Benchmark files have their own `${...}` substitution, so only sqllogictest files are checked
    if ext in SQLLOGICTEST_EXTENSIONS:
        lines, messages = replace_deprecated_syntax(full_path, lines)
        for message in messages:
            print(message, file=sys.stderr)

    # Filter out old metadata lines
    meta = ['#name:', '# name:', '#description:', '# description:', '#group:', '# group:']
    lines = [line for line in lines if not any(line.lower().startswith(m) for m in meta)]

    # Clean up empty leading lines
    while lines and not lines[0].strip():
        lines.pop(0)

    # Construct header
    header = [new_path_line]
    if found_description:
        header.append(new_description_line)
    header.append(new_group_line)
    header.append('\n')

    # Return formatted content
    return ''.join(header + lines), 0


def process_file(file_path, inplace, full_path=None):
    """Process a single file or stdin, either in-place or to stdout."""
    effective_path = full_path if file_path == '-' and full_path else file_path
    effective_path = effective_path.strip()

    if file_path == '-':
        if inplace:
            print("Error: -i cannot be used with stdin", file=sys.stderr)
            return 1
        lines = sys.stdin.readlines()
        formatted_content, status = format_file_content(effective_path, lines)
        if formatted_content is not None:
            sys.stdout.write(formatted_content)
        return status

    if not os.path.isfile(file_path):
        print(f"Error: {file_path} is not a file", file=sys.stderr)
        return 1

    with open_utf8(file_path, 'r') as f:
        lines = f.readlines()

    formatted_content, status = format_file_content(effective_path, lines)
    if formatted_content is None:
        return status

    if inplace:
        with open_utf8(file_path, 'w') as f:
            f.write(formatted_content)
    else:
        sys.stdout.write(formatted_content)

    return status


def main():
    """Main function to handle command-line arguments and process files."""
    parser = argparse.ArgumentParser(description='Format test files with standardized metadata headers.')
    parser.add_argument('files', nargs='+', help='Files to format (use - for stdin)')
    parser.add_argument('-i', '--inplace', action='store_true', help='Edit files in place')
    parser.add_argument('-f', '--full-path', help='Full path to use for stdin (only with -)')
    args = parser.parse_args()

    # Validate full-path usage
    if args.full_path and '-' not in args.files:
        print("Error: -f/--full-path can only be used with stdin (-)", file=sys.stderr)
        sys.exit(1)
    if args.full_path and len(args.files) > 1:
        print("Error: -f/--full-path cannot be used with multiple files", file=sys.stderr)
        sys.exit(1)

    exit_code = 0
    for file_path in args.files:
        status = process_file(file_path, args.inplace, args.full_path)
        exit_code = max(exit_code, status)

    sys.exit(exit_code)


if __name__ == '__main__':
    main()
