#!/usr/bin/env python3

import argparse
import json
import multiprocessing
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import time


# Minimum amount of evenly sized filename chunks.
TARGET_CHUNK_COUNT = 8
# Upper bound on the files in a chunk. clangd-tidy never closes the files it opens, so clangd keeps a preamble on
# disk for every file of a chunk - bounding the files per chunk bounds the peak size of the preamble cache
MAX_FILES_PER_CHUNK = 100

MAX_FAILED_CHUNKS = 3
MAX_RETRIES = 2
# How often a chunk that reported diagnostics is run again, with a fresh preamble cache, before it is reported.
# Real diagnostics reproduce - the phantom diagnostics clangd emits when it cannot write its preambles do not
MAX_DIAGNOSTIC_RETRIES = 1

ERROR_TAIL_LINES = 2000

FORCE_COLOR_OUTPUT = True
ANSI_RESET = '\033[0m'
ANSI_BOLD = '\033[1m'
ANSI_GRAY = '\033[90m'
ANSI_RED = '\033[31m'
ANSI_YELLOW = '\033[33m'
ANSI_CYAN = '\033[36m'

RETRY_BACKOFF_MS = 500
RETRY_PATTERNS = (
    'invalid header end',
    'broken pipe',
    'unexpected eof',
    'connection reset by peer',
    'failed to read message',
    'jsonrpc',
)

# clangd-tidy reports every diagnostic with its code in trailing brackets. clang-tidy checks are named
# "group-check-name", while clangd reports the diagnostics of the compiler itself under clang's internal
# names, which never contain a dash - e.g. "[undeclared_var_use]".
DIAGNOSTIC_CODE_REGEX = re.compile(r'^\S+:\d+:\d+: \w+: .*\[([A-Za-z0-9_.-]+)\]\s*$')


def color_text(text, color):
    if not FORCE_COLOR_OUTPUT:
        return text
    return f'{color}{text}{ANSI_RESET}'


def print_status(message, file=None):
    if file is None:
        file = sys.stdout
    print(color_text(message, ANSI_GRAY), file=file, flush=True)


def color_diagnostic_line(line):
    if re.search(r'\berror:', line, re.IGNORECASE):
        return color_text(line, ANSI_RED)
    if re.search(r'\bwarning:', line, re.IGNORECASE):
        return color_text(line, ANSI_YELLOW)
    if re.match(r'^[^:\s][^:]*:\d+:\d+:', line):
        return color_text(line, ANSI_BOLD)
    if re.match(r'^\s*\|?\s*\^', line):
        return color_text(line, ANSI_CYAN)
    return line


def make_absolute(path, directory):
    if os.path.isabs(path):
        return os.path.normpath(path)
    return os.path.normpath(os.path.join(directory, path))


def is_ignored_file(path, repo_root):
    normalized = os.path.normpath(path)
    try:
        relative = os.path.relpath(normalized, repo_root)
    except ValueError:
        return False
    return relative == 'third_party' or relative.startswith('third_party' + os.sep)


def chunk_files(files, target_chunk_count=TARGET_CHUNK_COUNT, max_files_per_chunk=MAX_FILES_PER_CHUNK):
    if not files:
        return
    chunk_count = max(target_chunk_count, (len(files) + max_files_per_chunk - 1) // max_files_per_chunk)
    chunk_count = min(chunk_count, len(files))
    chunk_size, remainder = divmod(len(files), chunk_count)
    start = 0
    for chunk_index in range(chunk_count):
        end = start + chunk_size
        if chunk_index < remainder:
            end += 1
        yield files[start:end]
        start = end


def load_files(build_path):
    database_path = os.path.join(build_path, 'compile_commands.json')
    repo_root = os.path.abspath(os.path.join(build_path, '..', '..'))
    with open(database_path, 'r', encoding='utf-8') as handle:
        database = json.load(handle)

    seen = set()
    files = []
    for entry in database:
        path = make_absolute(entry['file'], entry['directory'])
        if path in seen or is_ignored_file(path, repo_root):
            continue
        seen.add(path)
        files.append(os.path.relpath(path, repo_root))
    return repo_root, files


def create_attempt_clangd_wrapper(log_dir, clangd_binary, pch_dir, attempt_id):
    wrapper_path = os.path.join(log_dir, f'clangd-wrapper-attempt-{attempt_id}.sh')
    wrapper = f"""#!/bin/sh
set -eu
export TMPDIR={shlex.quote(pch_dir)}
export TMP={shlex.quote(pch_dir)}
export TEMP={shlex.quote(pch_dir)}
exec {shlex.quote(clangd_binary)} "$@" --pch-storage=disk
"""
    with open(wrapper_path, 'w', encoding='utf-8') as handle:
        handle.write(wrapper)
    os.chmod(wrapper_path, 0o755)
    return wrapper_path


def format_bytes(size):
    suffixes = ('B', 'KiB', 'MiB', 'GiB', 'TiB')
    value = float(size)
    for suffix in suffixes:
        if value < 1024 or suffix == suffixes[-1]:
            return f'{value:.1f} {suffix}'
        value /= 1024


def directory_size(path):
    total = 0
    if not os.path.exists(path):
        return total
    for root, _, files in os.walk(path):
        for filename in files:
            file_path = os.path.join(root, filename)
            try:
                total += os.path.getsize(file_path)
            except OSError:
                pass
    return total


def reset_pch_dir(pch_dir):
    shutil.rmtree(pch_dir, ignore_errors=True)
    os.makedirs(pch_dir, exist_ok=True)


def has_compiler_diagnostics(text):
    for line in (text or '').splitlines():
        match = DIAGNOSTIC_CODE_REGEX.match(line)
        if match and '-' not in match.group(1):
            return True
    return False


def is_retryable_failure(result):
    if result.returncode == 0:
        return False
    text = (result.stdout or '') + '\n' + (result.stderr or '')
    lower = text.lower()
    if any(pattern in lower for pattern in RETRY_PATTERNS):
        return True
    # a file that does not compile here does compile in every build job - the translation unit was parsed
    # against a broken preamble, which happens when the runner runs out of room for the preamble caches
    return has_compiler_diagnostics(text)


def print_output_tail(label, text):
    lines = (text or '').splitlines()
    if not lines:
        return
    shown = lines[-ERROR_TAIL_LINES:]
    omitted = len(lines) - len(shown)
    message = f'--- clangd-tidy {label} tail ({len(shown)} lines'
    if omitted > 0:
        message += f', {omitted} omitted'
    message += ') ---'
    print_status(message)
    for line in shown:
        print(color_diagnostic_line(line), flush=True)
    print_status(f'--- end clangd-tidy {label} tail ---')


def write_attempt_logs(log_dir, attempt_id, chunk, result):
    stdout_path = os.path.join(log_dir, f'attempt-{attempt_id}.stdout.log')
    stderr_path = os.path.join(log_dir, f'attempt-{attempt_id}.stderr.log')
    with open(stdout_path, 'w', encoding='utf-8') as handle:
        handle.write(result.stdout or '')
    with open(stderr_path, 'w', encoding='utf-8') as handle:
        handle.write(result.stderr or '')
    return stdout_path, stderr_path


def run_chunk_with_retries(base_command, chunk, repo_root, env, log_dir, pch_root, clangd_binary, attempt_counter):
    retries = 0
    diagnostic_retries = 0
    pch_dir = os.path.join(pch_root, 'current')
    os.makedirs(pch_dir, exist_ok=True)
    while True:
        attempt_counter[0] += 1
        attempt_id = attempt_counter[0]
        clangd_wrapper = create_attempt_clangd_wrapper(log_dir, clangd_binary, pch_dir, attempt_id)
        command = base_command + ['--clangd-executable', clangd_wrapper] + chunk
        start = time.monotonic()
        print_status(f'attempt {attempt_id} (retry {retries}/{MAX_RETRIES}, files {len(chunk)}): {chunk[0]}')
        result = subprocess.run(command, check=False, cwd=repo_root, env=env, text=True, capture_output=True)
        elapsed = time.monotonic() - start
        stdout_path, stderr_path = write_attempt_logs(log_dir, attempt_id, chunk, result)
        pch_size = format_bytes(directory_size(pch_dir))
        disk_free = format_bytes(shutil.disk_usage(pch_dir).free)
        print_status(
            f'attempt {attempt_id} finished exit={result.returncode} in {elapsed:.1f}s; '
            f'pch size: {pch_size}; disk free: {disk_free}'
        )
        if result.returncode == 0:
            if diagnostic_retries > 0:
                print_status(
                    f'attempt {attempt_id} passed with a fresh preamble cache - '
                    'the diagnostics of the previous attempt were phantom diagnostics'
                )
            # Clear the preambles before the next chunk. They are not reused across chunks, and letting them
            # accumulate fills the runner's disk, after which clangd reports phantom diagnostics.
            reset_pch_dir(pch_dir)
            return None
        retryable = is_retryable_failure(result)
        if not retryable and diagnostic_retries < MAX_DIAGNOSTIC_RETRIES:
            # clangd reports phantom diagnostics when it cannot write or read back its preambles - re-run the chunk
            # with a fresh preamble cache to tell those apart from diagnostics that are actually in the code
            diagnostic_retries += 1
            print_status(
                f'chunk reported diagnostics in attempt {attempt_id}; re-running it with a fresh preamble cache. '
                f'stdout: {stdout_path}; stderr: {stderr_path}'
            )
            reset_pch_dir(pch_dir)
            continue
        if retries >= MAX_RETRIES or not retryable:
            if retryable:
                print_status(
                    f'retry limit reached after attempt {attempt_id}; '
                    f'not retrying. stdout: {stdout_path}; stderr: {stderr_path}'
                )
            print_output_tail('stdout', result.stdout)
            print_output_tail('stderr', result.stderr)
            reset_pch_dir(pch_dir)
            return {
                'attempt_id': attempt_id,
                'stdout_path': stdout_path,
                'stderr_path': stderr_path,
                'retryable': retryable,
            }
        reset_pch_dir(pch_dir)
        delay = (RETRY_BACKOFF_MS / 1000.0) * (2**retries) + random.uniform(0.0, 0.2)
        print_status(
            f'retrying clangd-tidy after attempt {attempt_id}; retry {retries + 1}/{MAX_RETRIES} '
            f'in {delay:.2f}s. stdout: {stdout_path}; stderr: {stderr_path}'
        )
        time.sleep(delay)
        retries += 1


def process_chunk(
    base_command, chunk, chunk_index, chunk_count, repo_root, env, log_dir, pch_root, clangd_binary, attempt_counter
):
    failure = run_chunk_with_retries(
        base_command, chunk, repo_root, env, log_dir, pch_root, clangd_binary, attempt_counter
    )
    if not failure:
        return None
    failure.update(
        {
            'chunk_index': chunk_index,
            'chunk_count': chunk_count,
            'file_count': len(chunk),
            'first_file': chunk[0],
        }
    )
    return failure


def main():
    parser = argparse.ArgumentParser(description='Run clangd-tidy over all files in a compilation database.')
    parser.add_argument('-j', '--jobs', type=int, default=0, help='number of async workers passed to clangd-tidy')
    parser.add_argument('-p', '--build-path', default=None, help='path containing compile_commands.json')
    parser.add_argument(
        '--clangd-tidy-binary',
        default='clangd-tidy',
        help='path to the clangd-tidy executable',
    )
    parser.add_argument(
        '--clangd-binary',
        default='clangd',
        help='path to the clangd executable used by clangd-tidy',
    )
    parser.add_argument(
        '--query-driver',
        default=None,
        help='comma-separated list of query-driver globs passed to clangd-tidy',
    )
    args = parser.parse_args()

    build_path = os.path.abspath(args.build_path or os.getcwd())
    repo_root, files = load_files(build_path)
    if not files:
        print('No files found in compile_commands.json', file=sys.stderr)
        return 1

    jobs = args.jobs if args.jobs > 0 else multiprocessing.cpu_count()

    try:
        clangd_tidy_version = subprocess.check_output([args.clangd_tidy_binary, '--version'], text=True).splitlines()[0]
        clangd_version = subprocess.check_output([args.clangd_binary, '--version'], text=True).splitlines()[0]
    except Exception:
        print('Unable to run clangd-tidy or clangd.', file=sys.stderr)
        return 1
    print_status(f'Using clangd-tidy: {clangd_tidy_version}')
    print_status(f'Using clangd: {clangd_version}')

    log_dir = os.path.join(build_path, 'clangd-tidy-logs')
    os.makedirs(log_dir, exist_ok=True)
    pch_root = os.path.join(build_path, 'pchs')
    os.makedirs(pch_root, exist_ok=True)
    base_command = [
        args.clangd_tidy_binary,
        '--compile-commands-dir',
        build_path,
        '--jobs',
        str(jobs),
    ]
    if args.query_driver:
        base_command.extend(['--query-driver', args.query_driver])

    run_env = os.environ.copy()
    run_env['LC_ALL'] = 'C'
    run_env['PYTHONUNBUFFERED'] = '1'

    attempt_counter = [0]
    failed_chunks = []
    chunks = list(chunk_files(files))
    print_status(f'Running clangd-tidy on {len(files)} files in {len(chunks)} chunks')
    for chunk_index, chunk in enumerate(chunks, 1):
        failure = process_chunk(
            base_command,
            chunk,
            chunk_index,
            len(chunks),
            repo_root,
            run_env,
            log_dir,
            pch_root,
            args.clangd_binary,
            attempt_counter,
        )
        if failure:
            failed_chunks.append(failure)
            if len(failed_chunks) >= MAX_FAILED_CHUNKS:
                print_status(f'failed chunk limit reached ({MAX_FAILED_CHUNKS}); stopping clangd-tidy')
                break

    if failed_chunks:
        print_status('clangd-tidy failed chunks after retries:', file=sys.stderr)
        for failure in failed_chunks:
            print_status(
                f"  chunk {failure['chunk_index']}/{failure['chunk_count']}, "
                f"attempt {failure['attempt_id']}, files {failure['file_count']}, "
                f"first file: {failure['first_file']}, stdout: {failure['stdout_path']}, "
                f"stderr: {failure['stderr_path']}",
                file=sys.stderr,
            )
    print_status(f'clangd-tidy logs written to: {log_dir}')

    return 1 if failed_chunks else 0


if __name__ == '__main__':
    sys.exit(main())
