#!/usr/bin/env python3
"""Adds `make format-fix` functionality to .git/hooks/pre-commit."""

import os

HOOK_MARKER = '# make format-fix hook #'
HOOK_PATH = '.git/hooks/pre-commit'


FORMAT_SCRIPT = (
    'duckdb/scripts/format.py' if os.path.isfile('duckdb/scripts/format.py') else 'scripts/format.py'
)  # this is to support extension repos
FORMAT_CMD = f'python3 {FORMAT_SCRIPT} --staged --fix --noconfirm --modified-list "$modified_files"'

HOOK_BODY = f"""\

# make format-fix hook #
# format.py determines the staged files itself (skipping deleted files and files
# it does not format) and parallelises over them, so this is a single invocation
# rather than one per file.
modified_files=$(mktemp)
trap 'rm -f "$modified_files"' EXIT
{FORMAT_CMD} || exit 1
if [ -s "$modified_files" ]; then
    # By default the formatting is left unstaged and the commit is aborted: staging
    # it automatically would also sweep in changes that were deliberately left
    # unstaged (e.g. after a `git add -p`). Set DUCKDB_FORMAT_HOOK_RESTAGE=1 to
    # stage the reformatted files and let the commit proceed.
    if [ "${{DUCKDB_FORMAT_HOOK_RESTAGE:-0}}" = "1" ]; then
        xargs -0 -r git add -- <"$modified_files"
    else
        echo ""
        echo "The formatter reformatted the following files:"
        tr '\\0' '\\n' <"$modified_files" | while read -r file; do
            echo "  $file"
        done
        echo ""
        echo "The changes were left unstaged. Review them, stage them with 'git add'"
        echo "and commit again, or set DUCKDB_FORMAT_HOOK_RESTAGE=1 to stage them"
        echo "automatically."
        exit 1
    fi
fi
"""


def main():
    if os.path.isfile(HOOK_PATH):
        with open(HOOK_PATH, 'r') as f:
            content = f.read()
        if HOOK_MARKER in content:
            print("Hook already exists, no action needed. exiting.")
        else:
            print(
                ".git/hooks/pre-commit file exists but does not contain make `make format-fix` functionality for staged files"
            )
            print("Adding `make format-fix` functionality for staged files")
            with open(HOOK_PATH, 'a') as f:
                f.write(HOOK_BODY)
    else:
        print("Creating .git/hooks/pre-commit file with `make format-fix` functionality for staged files")
        with open(HOOK_PATH, 'w') as f:
            f.write('#!/bin/sh\n' + HOOK_BODY)
        os.chmod(HOOK_PATH, 0o755)
    print("Done! ✅")


if __name__ == '__main__':
    main()
