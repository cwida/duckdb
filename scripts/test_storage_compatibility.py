import argparse
import concurrent.futures
import csv
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description='Run a full benchmark using the CLI and report the results.')
group = parser.add_mutually_exclusive_group(required=True)
group.add_argument('--old-cli', action='store', help='Path to the CLI of the old DuckDB version to test')
group.add_argument('--versions', type=str, action='store', help='DuckDB versions to test')
parser.add_argument('--new-unittest', action='store', help='Path to the new unittester to run', required=True)
parser.add_argument('--new-cli', action='store', help='Path to the new unittester to run', default=None)
parser.add_argument('--compatibility', action='store', help='Storage compatibility version', default='v1.0.0')
parser.add_argument(
    '--test-config', action='store', help='Test config script to run', default='test/configs/storage_compatibility.json'
)
parser.add_argument('--db-name', action='store', help='Database name to write to', default='bwc_storage_test.db')
parser.add_argument('--workers', action='store', help='Number of workers or CPU percentage to use', default='75%')
parser.add_argument('--abort-on-failure', action='store_true', help='Abort on first failure', default=False)
parser.add_argument('--start-offset', type=int, action='store', help='Test start offset', default=None)
parser.add_argument('--end-offset', type=int, action='store', help='Test end offset', default=None)
parser.add_argument('--no-summarize-failures', action='store_true', help='Skip failure summary', default=False)
parser.add_argument('--list-versions', action='store_true', help='Only list versions to test', default=False)
parser.add_argument(
    '--run-empty-tests',
    action='store_true',
    help='Run tests that don' 't have a CREATE TABLE or CREATE VIEW statement',
    default=False,
)

args, extra_args = parser.parse_known_args()


def resolve_workers(workers):
    cpu_count = os.cpu_count() or 1
    workers = workers.strip()
    if workers.endswith("%"):
        percentage = int(workers[:-1])
        return max(1, int(cpu_count * (percentage / 100.0)))
    return max(1, int(workers))


programs_to_test = []
if args.versions is not None:
    version_splits = args.versions.split('|')
    for version in version_splits:
        cli_path = os.path.join(Path.home(), '.duckdb', 'cli', version, 'duckdb')
        if not os.path.isfile(cli_path):
            if os.system(f'curl https://install.duckdb.org | DUCKDB_VERSION={version} sh'):
                raise Exception(f"CURL install for DuckDB version: {version} failed")
        programs_to_test.append(os.path.abspath(cli_path))
else:
    programs_to_test.append(os.path.abspath(args.old_cli))

unittest_program = os.path.abspath(args.new_unittest)
db_name = args.db_name
test_config = os.path.abspath(args.test_config)
new_cli = (
    os.path.abspath(args.new_unittest.replace('test/unittest', 'duckdb'))
    if args.new_cli is None
    else os.path.abspath(args.new_cli)
)
summarize_failures = not args.no_summarize_failures
workers = resolve_workers(args.workers)
repo_root = os.getcwd()

# Ask the extension runner to output the tests selected by the storage config.
proc = subprocess.run(
    [unittest_program, '--test-config', test_config, '--list-tests'] + extra_args,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
stdout = proc.stdout.decode('utf8', errors='backslashreplace').strip()
stderr = proc.stderr.decode('utf8', errors='backslashreplace').strip()
if len(stderr) > 0:
    print("Failed to run program " + unittest_program)
    print("Returncode:", proc.returncode)
    print(stdout)
    print(stderr)
    exit(1)


# The output is in the format of 'PATH\tGROUP', we're only interested in the PATH portion
test_cases = []
first_line = True
for line in stdout.splitlines():
    if first_line:
        first_line = False
        continue
    if len(line.strip()) == 0:
        continue
    splits = line.rsplit('\t', 1)
    test_cases.append(splits[0])

test_cases.sort()
if args.compatibility != 'v1.0.0':
    raise Exception("Only v1.0.0 is supported for now (FIXME)")


def escape_cmd_arg(arg):
    if '"' in arg or '\'' in arg or ' ' in arg or '\\' in arg:
        arg = arg.replace('\\', '\\\\')
        arg = arg.replace('"', '\\"')
        arg = arg.replace("'", "\\'")
        return f'"{arg}"'
    return arg


def format_cmd(cmd):
    return ' '.join(escape_cmd_arg(entry) for entry in cmd)


error_container = []
passed_count = 0
skipped_count = 0
failed_count = 0


def handle_failure(test, cmd, msg, stdout, stderr, returncode):
    print(f"==============FAILURE============", file=sys.stderr)
    print(test, file=sys.stderr)
    print(f"==============MESSAGE============", file=sys.stderr)
    print(msg, file=sys.stderr)
    print(f"==============REPRODUCE==========", file=sys.stderr)
    cmd_str = format_cmd(cmd)
    print(cmd_str, file=sys.stderr)
    print(f"==============RETURNCODE=========", file=sys.stderr)
    print(str(returncode), file=sys.stderr)
    print(f"==============STDOUT=============", file=sys.stderr)
    print(stdout, file=sys.stderr)
    print(f"==============STDERR=============", file=sys.stderr)
    print(stderr, file=sys.stderr)
    print(f"=================================", file=sys.stderr)
    error_container.append({'test': test, 'stderr': stderr})


def make_failure(test, cmd, msg, stdout='', stderr='', returncode=1):
    return {
        'test': test,
        'cmd': cmd,
        'msg': msg,
        'stdout': stdout,
        'stderr': stderr,
        'returncode': returncode,
    }


def run_program(test, cmd, description):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout = proc.stdout.decode('utf8', errors='backslashreplace').strip()
    stderr = proc.stderr.decode('utf8', errors='backslashreplace').strip()
    if proc.returncode != 0:
        return {
            'test': test,
            'cmd': cmd,
            'msg': f'Failed to {description}',
            'stdout': stdout,
            'stderr': stderr,
            'returncode': proc.returncode,
        }
    return None


def matches_skip_error(failure, skip_error_messages):
    if not skip_error_messages:
        return False
    combined = failure.get('stderr', '') + failure.get('stdout', '')
    return any(msg in combined for msg in skip_error_messages)


def execute_test(i, test):
    test_path = test if os.path.isabs(test) else os.path.join(repo_root, test)
    skipped = False
    if not args.run_empty_tests:
        with open(test_path, 'r', encoding='utf8', errors='backslashreplace') as f:
            test_contents = f.read().lower()
        if 'create table' not in test_contents and 'create view' not in test_contents:
            skipped = True

    result = {
        'index': i,
        'test': test,
        'skipped': skipped,
        'messages': [],
        'failures': [],
    }
    if skipped:
        return result

    with tempfile.TemporaryDirectory(prefix='storage_compat_') as temp_dir:
        db_path = os.path.join(temp_dir, db_name)
        table_list_path = os.path.join(temp_dir, 'table_list.csv')
        test_temp_dir = os.path.join(temp_dir, 'test_temp_dir')
        worker_test_config = os.path.join(temp_dir, 'storage_compatibility.json')
        with open(test_config, 'r') as f:
            config_contents = json.load(f)
        skip_error_messages = config_contents.get('skip_error_messages', [])
        config_contents['initial_db'] = db_path
        with open(worker_test_config, 'w') as f:
            json.dump(config_contents, f)
        # The durable db (initial_db) lives outside test_temp_dir, so destroy is indifferent;
        # pin test_temp_dir as the exact root with both levels off (replaces --test-temp-dir).
        # The enclosing TemporaryDirectory cleans everything up, so destroy=never just retains
        # until then.
        unittest_cmd = [
            unittest_program,
            '--test-config',
            worker_test_config,
            '--test-flags',
            shlex.join(
                [
                    '--temp-dir-root',
                    test_temp_dir,
                    '--temp-dir-run-id',
                    'off',
                    '--temp-dir-test-id',
                    'off',
                    '--temp-dir-destroy',
                    'never',
                ]
            ),
            test,
        ]
        failure = run_program(
            test,
            unittest_cmd,
            'Run Test',
        )
        if failure is not None:
            if matches_skip_error(failure, skip_error_messages):
                result['skipped'] = True
                return result
            result['failures'].append(failure)
            return result

        if not os.path.isfile(db_path):
            result['failures'].append(
                make_failure(
                    test,
                    unittest_cmd,
                    f'Failed to create a database file by the name of {db_path}',
                )
            )
            return result

        list_tables_cmd = [
            programs_to_test[-1],
            db_path,
            '-c',
            '.headers off',
            '-csv',
            '-c',
            f'.output {table_list_path}',
            '-c',
            'SHOW ALL TABLES',
        ]
        failure = run_program(test, list_tables_cmd, 'List Tables')
        if failure is not None:
            if matches_skip_error(failure, skip_error_messages):
                result['skipped'] = True
                return result
            result['failures'].append(failure)
            return result

        tables = []
        with open(table_list_path, newline='') as f:
            reader = csv.reader(f)
            for row in reader:
                tables.append((row[1], row[2]))
        if len(tables) == 0:
            result['skipped'] = True
            result['messages'].append("No tables/views were created, skipping")
            return result

        failures = []
        last_cmd = None
        for cli in programs_to_test:
            cmd = [cli, db_path]
            for table in tables:
                schema_name = table[0].replace('"', '""')
                table_name = table[1].replace('"', '""')
                cmd += ['-c', f'FROM "{schema_name}"."{table_name}"']
            last_cmd = cmd
            failure = run_program(test, cmd, 'Query Tables')
            if failure is not None:
                failures.append(failure)
        if len(failures) > 0:
            failures = [f for f in failures if not matches_skip_error(f, skip_error_messages)]
        if len(failures) > 0:
            # A query failure can be expected for stale views. Only report it
            # when the same query succeeds against the new CLI.
            new_cmd = [new_cli] + last_cmd[1:]
            new_failure = run_program(test, new_cmd, 'Query Tables (New)')
            if new_failure is None:
                result['failures'].extend(failures)
        return result


def process_result(result):
    global passed_count, skipped_count, failed_count
    if result['skipped']:
        skipped_count += 1
        return

    i = result['index']
    test = result['test']
    if result['failures'] or result['messages']:
        failed_count += 1
        print(f'[{i}/{len(test_cases)}]: {test}')
    else:
        passed_count += 1
    for failure in result['failures']:
        handle_failure(**failure)
    for message in result['messages']:
        print(message)


start = 0 if args.start_offset is None else args.start_offset
end = len(test_cases) if args.end_offset is None else args.end_offset
selected_tests = list(enumerate(test_cases[start:end], start=start))
start_time = time.monotonic()

with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
    future_to_test = {}
    next_test_idx = 0

    while next_test_idx < len(selected_tests) and len(future_to_test) < workers:
        i, test = selected_tests[next_test_idx]
        future_to_test[executor.submit(execute_test, i, test)] = (i, test)
        next_test_idx += 1

    stop_launching = False
    while future_to_test:
        done, _ = concurrent.futures.wait(future_to_test, return_when=concurrent.futures.FIRST_COMPLETED)
        for future in done:
            future_to_test.pop(future)
            result = future.result()
            process_result(result)
            if args.abort_on_failure and len(error_container) > 0:
                stop_launching = True
        while not stop_launching and next_test_idx < len(selected_tests) and len(future_to_test) < workers:
            i, test = selected_tests[next_test_idx]
            future_to_test[executor.submit(execute_test, i, test)] = (i, test)
            next_test_idx += 1

elapsed = time.monotonic() - start_time
print(f'tests took {elapsed:.0f}s: {passed_count} passed, {skipped_count} skipped, {failed_count} failed')

if len(error_container) == 0:
    exit(0)

if summarize_failures:
    print(
        '''\n\n====================================================
================  FAILURES SUMMARY  ================
====================================================\n
''',
        file=sys.stderr,
    )
    for i, error in enumerate(error_container, start=1):
        print(f"\n{i}:", error["test"], "\n", file=sys.stderr)
        print(error["stderr"], file=sys.stderr)

exit(1)
