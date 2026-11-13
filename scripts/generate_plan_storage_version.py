# this script re-generates the binary file used for Test deserialized plans from file
# before running this script, increment the version number in src/planner/logical_operator.cpp and
# recompile (make debug)
# Note that the test is not linked unless tpch is included in DUCKDB_USE

import os
import subprocess
from python_helpers import open_utf8

shell_proc = os.path.join('build', 'debug', 'test', 'unittest')
gen_binary_file = os.path.join('test', 'api', 'serialized_plans', 'serialized_plans.binary')


def try_remove_file(fname):
    try:
        os.remove(fname)
    except:
        pass


try_remove_file(gen_binary_file)


def run_test(test):
    print(test)
    env = os.environ.copy()
    env["GEN_PLAN_STORAGE"] = "1"
    res = subprocess.run([shell_proc, test], capture_output=True, env=env)
    stdout = res.stdout.decode('utf8').strip()
    stderr = res.stderr.decode('utf8').strip()
    if res.returncode != 0:
        print("Failed to create binary file!")
        print("----STDOUT----")
        print(stdout)
        print("----STDERR----")
        print(stderr)


run_test("Generate serialized plans file")
