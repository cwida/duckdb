import subprocess
import time
import argparse
import os
import signal

parser = argparse.ArgumentParser(description='Runs the memory leak tests')

parser.add_argument(
    '--unittest',
    dest='unittest',
    action='store',
    help='Path to the unittest runner',
    default='build/release/test/unittest',
)
parser.add_argument('--test', dest='test', action='store', help='The name of the tests to run (* is all)', default='*')
parser.add_argument(
    '--timeout',
    dest='timeout',
    action='store',
    type=int,
    help='The maximum time to run the test and measure memory usage (in seconds)',
    default=60,
)
parser.add_argument(
    '--threshold-percentage',
    dest='threshold_percentage',
    action='store',
    type=float,
    help='The percentage threshold before we consider an increase a regression',
    default=0.01,
)
parser.add_argument(
    '--threshold-absolute',
    dest='threshold_absolute',
    action='store',
    type=float,
    help='The absolute threshold before we consider an increase a regression',
    default=1000,
)
parser.add_argument('--verbose', dest='verbose', action='store', help='Verbose output', default=True)

args = parser.parse_args()

unittest_program = args.unittest
test_filter = args.test
test_time = args.timeout
verbose = args.verbose
measurements_per_second = 1.0

# get a list of all unittests
proc = subprocess.Popen(
    [unittest_program, '--list-tests', '[memoryleak]'], stdout=subprocess.PIPE, stderr=subprocess.PIPE
)
stdout_bytes, stderr_bytes = proc.communicate()
stdout = stdout_bytes.decode('utf8')
stderr = stderr_bytes.decode('utf8')
if proc.returncode != 0:
    print("Failed to run program " + unittest_program)
    print(proc.returncode)
    print(stdout)
    print(stderr)
    exit(1)

test_cases = []
first_line = True
for line in stdout.splitlines():
    if first_line:
        first_line = False
        continue
    if len(line.strip()) == 0:
        continue
    splits = line.rsplit('\t', 1)
    if test_filter == '*' or test_filter in splits[0]:
        test_cases.append(splits[0])

if len(test_cases) == 0:
    print(f"No tests matching filter \"{test_filter}\" found")
    exit(0)


def sizeof_fmt(num, suffix="B"):
    for unit in ["", "K", "M", "G", "T", "P", "E", "Z"]:
        if abs(num) < 1000.0:
            return f"{num:3.1f}{unit}{suffix}"
        num /= 1000.0
    return f"{num:.1f}Yi{suffix}"


def run_test(test_case):
    # Measure and stop the compatibility runner and its DuckDB child together.
    proc = subprocess.Popen(
        [unittest_program, '--test-flags=--memory-leak-tests', test_case],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    pid = proc.pid

    leak = True
    rss = []
    try:
        for i in range(int(test_time * measurements_per_second)):
            time.sleep(1.0 / measurements_per_second)
            if proc.poll() is not None:
                print("------------------------------------------------")
                print("                    FAILURE                     ")
                print("------------------------------------------------")
                print("                    stdout:                     ")
                print("------------------------------------------------")
                print(proc.stdout.read().decode('utf8'))
                print("------------------------------------------------")
                print("                    stderr:                     ")
                print("------------------------------------------------")
                print(proc.stderr.read().decode('utf8'))
                exit(1)
            ps_proc = subprocess.run(
                ['ps', '-e', '-o', 'pid=', '-o', 'ppid=', '-o', 'rss='],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            processes = {}
            for line in ps_proc.stdout.splitlines():
                process_pid, parent_pid, rss_kib = (int(value) for value in line.split())
                processes[process_pid] = (parent_pid, rss_kib)
            process_ids = {pid}
            changed = True
            while changed:
                changed = False
                for process_pid, (parent_pid, _) in processes.items():
                    if process_pid not in process_ids and parent_pid in process_ids:
                        process_ids.add(process_pid)
                        changed = True
            memory_usage_in_bytes = sum(
                processes[process_id][1] for process_id in process_ids if process_id in processes
            )
            memory_usage_in_bytes *= 1024
            if verbose:
                print(f"{i / measurements_per_second}: {sizeof_fmt(memory_usage_in_bytes)}")

            rss.append(memory_usage_in_bytes)
            if not has_memory_leak(rss):
                leak = False
                break
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
        proc.wait()
    if leak:
        print("------------------------------------------------")
        print("                     ERROR                      ")
        print("------------------------------------------------")
        print(f"Memory leak detected in test case \"{test_case}\"")
        print("------------------------------------------------")
    elif verbose:
        print("------------------------------------------------")
        print("                    Success!                    ")
        print("------------------------------------------------")
        print("------------------------------------------------")
        print(f"No memory leaks detected in test case \"{test_case}\"")
        print("------------------------------------------------")
    if verbose or leak:
        print("Observed memory usages")
        print("------------------------------------------------")
        for i in range(len(rss)):
            memory = rss[i]
            print(f"{i / measurements_per_second}: {sizeof_fmt(memory)}")
        if leak:
            exit(1)


def has_memory_leak(rss):
    measurement_count = int(measurements_per_second * 10)
    if len(rss) <= measurement_count:
        # not enough measurements yet
        return True
    differences = []
    for i in range(1, len(rss)):
        differences.append(rss[i] - rss[i - 1])
    max_memory = max(rss)
    sum_differences = sum(differences[-measurement_count:])
    return sum_differences > (max_memory * args.threshold_percentage + args.threshold_absolute)


for index, test in enumerate(test_cases):
    print(f"[{index}/{len(test_cases)}] {test}")
    run_test(test)
