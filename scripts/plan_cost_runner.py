import argparse
import glob
import json
import os
import subprocess
import sys

if __package__:
    from .regression.local_extensions import extension_loading_args
else:
    from regression.local_extensions import extension_loading_args


OLD_DB_NAME = "old.duckdb"
NEW_DB_NAME = "new.duckdb"
PROFILE_FILENAME = "duckdb_profile.json"

ENABLE_PROFILING = "PRAGMA enable_profiling=json"
PROFILE_OUTPUT = f"PRAGMA profile_output='{PROFILE_FILENAME}'"

BANNER_SIZE = 52
RETRIES = 2


def run_command(command, **kwargs):
    return subprocess.run(
        command,
        check=False,
        **kwargs,
    )


def run_sql_file(cli, dbname, sql_file, extensions=None, **kwargs):
    command = [cli, *extension_loading_args(cli, extensions or []), dbname]
    with open(sql_file, 'rb') as sql_input:
        return run_command(command, stdin=sql_input, **kwargs)


def init_db(cli, dbname, benchmark_dir, extensions=None):
    print(f"INITIALIZING {dbname} ...")
    attempts = RETRIES + 1
    if os.path.exists(dbname):
        os.remove(dbname)

    for attempt in range(1, attempts + 1):
        completed = run_sql_file(
            cli,
            dbname,
            f"{benchmark_dir}/init/schema.sql",
            stdout=subprocess.DEVNULL,
        )
        if completed.returncode == 0:
            completed = run_sql_file(
                cli,
                dbname,
                f"{benchmark_dir}/init/load.sql",
                extensions,
                stdout=subprocess.DEVNULL,
            )
            if completed.returncode == 0:
                print("INITIALIZATION DONE")
                return

        if attempt < attempts:
            if os.path.exists(dbname):
                os.remove(dbname)
            print(
                f"Retrying failure in init db ({dbname}) "
                f"(attempt {attempt + 1}/{attempts}, return code {completed.returncode})"
            )
            continue

        raise subprocess.CalledProcessError(
            completed.returncode,
            completed.args,
            output=completed.stdout,
            stderr=completed.stderr,
        )


class PlanCost:
    def __init__(self):
        self.total = 0
        self.build_side = 0
        self.probe_side = 0
        self.time = 0

    def __add__(self, other):
        self.total += other.total
        self.build_side += other.build_side
        self.probe_side += other.probe_side
        return self

    def __gt__(self, other):
        if self == other or self.total < other.total:
            return False
        # if the total intermediate cardinalities is greater, also inspect time.
        # it's possible a plan reordering increased cardinalities, but overall execution time
        # was not greatly affected
        total_card_increased = self.total > other.total
        build_card_increased = self.build_side > other.build_side
        if total_card_increased and build_card_increased:
            return True
        # we know the total cardinality is either the same or higher and the build side has not increased
        # in this case fall back to the timing. It's possible that even if the probe side is higher
        # since the tuples are in flight, the plan executes faster
        return self.time > other.time * 1.03

    def __lt__(self, other):
        if self == other:
            return False
        return not (self > other)

    def __eq__(self, other):
        return self.total == other.total and self.build_side == other.build_side and self.probe_side == other.probe_side


def get_physical_operator_type(op) -> str:
    # New format uses 'type' for the physical operator type (e.g. "HASH_JOIN")
    if 'type' in op:
        return op['type']
    # Legacy format (legacy_metrics_format=true) uses 'operator_name'
    if 'operator_name' in op:
        return op['operator_name']
    # Old format (pre-metricsrefactor) stored the physical type in 'name'
    return op.get('name', '')


def get_operator_cardinality(op) -> int:
    # New format uses 'intermediate_rows', old format uses 'operator_cardinality'
    if 'intermediate_rows' in op:
        return op['intermediate_rows']
    return op.get('operator_cardinality', 0)


def has_operator_cardinality(op) -> bool:
    return 'intermediate_rows' in op or 'operator_cardinality' in op


def get_root_operator(data) -> dict:
    if 'operator' in data:
        return data['operator'][0]
    # main branch (pre-rename) uses 'operator_info'
    if 'operator_info' in data:
        return data['operator_info'][0]
    return data['children'][0]


def is_measured_join(op) -> bool:
    if get_physical_operator_type(op) != 'HASH_JOIN':
        return False
    extra_info = op.get('extra_info', {})
    if 'Join Type' not in extra_info:
        return False
    if extra_info['Join Type'].startswith('MARK'):
        return False
    return True


def op_inspect(op) -> PlanCost:
    cost = PlanCost()
    if is_measured_join(op):
        cost.total = get_operator_cardinality(op)
        if has_operator_cardinality(op['children'][0]):
            cost.probe_side += get_operator_cardinality(op['children'][0])
        if has_operator_cardinality(op['children'][1]):
            cost.build_side += get_operator_cardinality(op['children'][1])

        left_cost = op_inspect(op['children'][0])
        right_cost = op_inspect(op['children'][1])
        cost.probe_side += left_cost.probe_side + right_cost.probe_side
        cost.build_side += left_cost.build_side + right_cost.build_side
        cost.total += left_cost.total + right_cost.total
        return cost

    for child_op in op.get('children', []):
        cost += op_inspect(child_op)

    return cost


def query_plan_cost(cli, dbname, query):
    try:
        command = [cli, '--readonly', dbname, '-c', f"{ENABLE_PROFILING};{PROFILE_OUTPUT};{query}"]
        attempts = RETRIES + 1
        for attempt in range(1, attempts + 1):
            completed = run_command(
                command,
                capture_output=True,
            )
            if completed.returncode == 0:
                break
            if attempt < attempts:
                print(
                    f"Retrying failure in query profiling ({dbname}) "
                    f"(attempt {attempt + 1}/{attempts}, return code {completed.returncode})"
                )
                continue
            raise subprocess.CalledProcessError(
                completed.returncode,
                completed.args,
                output=completed.stdout,
                stderr=completed.stderr,
            )
    except subprocess.CalledProcessError as e:
        print("-------------------------")
        print("--------Failure----------")
        print("-------------------------")
        print(e.stderr.decode('utf8'))
        print("-------------------------")
        print("--------Output----------")
        print("-------------------------")
        print(e.output.decode('utf8'))
        print("-------------------------")
        raise e
    with open(PROFILE_FILENAME, 'r') as file:
        data = json.load(file)
        return op_inspect(get_root_operator(data))


def print_banner(text):
    text_len = len(text)
    rest = BANNER_SIZE - text_len - 10
    l_width = int(rest / 2)
    r_width = l_width
    if rest % 2 != 0:
        l_width += 1
    print("")
    print("=" * BANNER_SIZE)
    print("=" * l_width + " " * 5 + text + " " * 5 + "=" * r_width)
    print("=" * BANNER_SIZE)


def print_diffs(diffs):
    for query_name, old_cost, new_cost in diffs:
        print("")
        print("Query:", query_name)
        print("Old total cost:", old_cost.total)
        print("Old build cost:", old_cost.build_side)
        print("Old probe cost:", old_cost.probe_side)
        print("New total cost:", new_cost.total)
        print("New build cost:", new_cost.build_side)
        print("New probe cost:", new_cost.probe_side)


def main():
    parser = argparse.ArgumentParser(description="Plan cost regression test script with old and new versions.")

    parser.add_argument("--old", type=str, help="Path to the old runner.", required=True)
    parser.add_argument("--new", type=str, help="Path to the new runner.", required=True)
    parser.add_argument("--dir", type=str, help="Path to the benchmark directory.", required=True)
    parser.add_argument("--extension", action="append", default=[], help="Extension required to initialize the data.")

    args = parser.parse_args()

    old = args.old
    new = args.new
    benchmark_dir = args.dir

    init_db(old, OLD_DB_NAME, benchmark_dir, args.extension)
    init_db(new, NEW_DB_NAME, benchmark_dir, args.extension)

    improvements = []
    regressions = []

    files = glob.glob(f"{benchmark_dir}/queries/*.sql")
    files.sort()

    print("")
    print("RUNNING BENCHMARK QUERIES")
    for f in files:
        query_name = f.split("/")[-1].replace(".sql", "")

        with open(f, "r") as file:
            query = file.read()

        old_cost = query_plan_cost(old, OLD_DB_NAME, query)
        new_cost = query_plan_cost(new, NEW_DB_NAME, query)

        if old_cost > new_cost:
            improvements.append((query_name, old_cost, new_cost))
        elif new_cost > old_cost:
            regressions.append((query_name, old_cost, new_cost))

    exit_code = 0
    if improvements:
        print_banner("IMPROVEMENTS DETECTED")
        print_diffs(improvements)
    if regressions:
        exit_code = 1
        print_banner("REGRESSIONS DETECTED")
        print_diffs(regressions)
    if not improvements and not regressions:
        print_banner("NO DIFFERENCES DETECTED")

    os.remove(OLD_DB_NAME)
    os.remove(NEW_DB_NAME)
    os.remove(PROFILE_FILENAME)

    exit(exit_code)


if __name__ == "__main__":
    main()
