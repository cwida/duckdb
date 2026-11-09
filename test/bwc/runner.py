import shutil
import argparse
from utils.duckdb_cli import DuckDBCLI
from utils.duckdb_installer import install_assets, install_extensions, make_cli_path, get_version
from utils.test_files_parser import load_test_files
from utils.test_report import TestReport
from utils.logger import make_logger
from utils.version_list import list_supported_duckdb_versions
import time
from concurrent.futures import ThreadPoolExecutor
from threading import Lock, Event
from os.path import abspath, dirname
import os
import sys
import duckdb

logger = make_logger(__name__)
cancel_event = Event()
has_checked_versions = False

parser = argparse.ArgumentParser(description='Runs the serialization BWC tests')

parser.add_argument(
    '--cleanup_bwc_directory',
    dest='cleanup_bwc_directory',
    action='store',
    help='When set, the script will clean up the BWC runtime directory. Use with `dry_run=False` argument to actually delete the files.',
    default=False,
)

parser.add_argument(
    '--dry_run',
    dest='dry_run',
    action='store',
    help='When set, the cleanup of the BWC runtime directory will actually delete the files instead of just logging them.',
    default=True,
)

parser.add_argument(
    '--test_pattern',
    dest='test_pattern',
    action='store',
    help='When set, only tests whose relative path contains the given pattern will be run. Example: `--test_pattern=window` will only run tests that have "window" in their path.',
    default=None,
)

parser.add_argument(
    '--stop_on_failure',
    dest='stop_on_failure',
    action='store',
    help='When set, the test runner will stop executing further tests after the first failure. This can be useful when debugging a specific test or when you want to quickly identify if there are any issues without running the entire suite.',
    default=False,
)

parser.add_argument(
    '--run_sequentially',
    dest='run_sequentially',
    action='store',
    help='When set, the test runner will execute tests sequentially instead of in parallel. This can be useful for debugging or when running a subset of tests to get more detailed logs.',
    default=False,
)

parser.add_argument(
    '--old_duckdb_version',
    dest='old_duckdb_version',
    action='store',
    help='The old DuckDB version to test against. If not set, tests will be run for all supported versions.',
    default=None,
)

parser.add_argument(
    '--max_workers',
    dest='max_workers',
    action='store',
    help='The maximum number of worker threads to use when running tests in parallel. Default is 24.',
    type=int,
    default=24,
)

parser.add_argument(
    '--new_cli_path',
    dest='new_cli_path',
    action='store',
    help='Path to the new DuckDB CLI to test. If not set, it will default to "build/release/duckdb" in the repo.',
    default=None,
)

parser.add_argument(
    '--test_file',
    dest='test_file',
    action='store',
    help='Path to a test file to run',
    default=None,
)

args = parser.parse_args()


class TestRunnerContext:
    def __init__(self, old_duckdb_version, new_cli_path, nb_tests, summary_file, report_con):
        self.old_cli_path = make_cli_path(old_duckdb_version)
        self.new_cli_path = new_cli_path
        self.nb_tests = nb_tests
        self.summary_file = summary_file
        self.report_con = report_con
        self.report_lock = Lock()

    def initialize(self):
        self.old_cli_v = '-'.join(get_version(self.old_cli_path))
        logger.info(f"Old CLI path: {self.old_cli_path} @ {self.old_cli_v}")

        self.new_cli_v = '-'.join(get_version(self.new_cli_path))
        logger.info(f"New CLI path: {self.new_cli_path} @ {self.new_cli_v}")


def run_multi_clis(clis, c):
    results = []
    for cli in clis:
        r = cli.execute_command(c)
        results.append(r)
    return results


TRANSIENT_ERROR_PATTERNS = [
    "Could not set lock on file",
    "HTTP",
    "Connection",
    "Unable to connect",
]


def is_transient_error(output):
    error = output.get("error") or ""
    exception = output.get("exception_message") or ""
    combined = error + exception
    return len(combined) == 0 or any(pattern in combined for pattern in TRANSIENT_ERROR_PATTERNS)


def run_step(cli, report, func_name, *args):
    str_args = ", ".join([f"'{arg}'" for arg in args])
    command = f"CALL {func_name}({str_args});"
    output = cli.execute_command(command)

    # Retry on transient errors (file locks, network issues)
    if not output["success"] and is_transient_error(output):
        for attempt in range(1, 4):
            logger.warning(f"[{cli.version}] Transient error, retrying in {attempt}s (attempt {attempt}/3)")
            time.sleep(attempt)
            output = cli.execute_command(command)
            if output["success"] or not is_transient_error(output):
                break

    # We had to name the functions `tu_compare_results_from_xxx`
    # in DuckDB 1.1.x to avoid collision and support overloading.
    step_name = "compare_results" if "compare_results" in func_name else func_name
    return report.end_step(step_name, output)


def same_extension_version(a, b):
    # Builds abbreviate the commit hash to different lengths, e.g. 2669706 and 2669706d33
    shorter, longer = sorted([a.strip(), b.strip()], key=len)
    return shorter == longer or (len(shorter) >= 7 and longer.startswith(shorter))


def get_extensions_versions(clis):
    results = run_multi_clis(
        clis, "SELECT extension_version FROM duckdb_extensions() WHERE extension_name='test_utils';"
    )
    versions = []
    for r in results:
        if not r["success"]:
            raise RuntimeError(f"Failed to get extension version: {r['error']}")
        # Find the actual version value, skipping the CSV header and any stale output
        version = None
        for line in r["output"]:
            if line and line != "extension_version":
                version = line
                break
        if not version:
            raise RuntimeError(
                f"test-utils extension not loaded for CLI '{clis[len(versions)].version}' - output: {r['output']}"
            )
        versions.append(version)
    return versions


def do_run_test(ctx, test_spec):
    global has_checked_versions
    logger.info(f"Running test {test_spec.test_idx}/{ctx.nb_tests}: {test_spec.test_spec_relative_path}")
    report = TestReport(test_spec, ctx.old_cli_v, ctx.new_cli_v)
    sanity_checks = False
    with DuckDBCLI(ctx.old_cli_path, unsigned=True) as old_cli:
        with DuckDBCLI(ctx.new_cli_path, unsigned=True) as new_cli:
            ext_path = 'test_utils'
            run_multi_clis([old_cli, new_cli], f".cd {test_spec.test_runtime_directory}\nLOAD '{ext_path}'")
            logger.debug(f"Loaded extension from '{ext_path}'")
            if sanity_checks:
                run_multi_clis([old_cli, new_cli], "pragma version;")
                run_multi_clis(
                    [old_cli, new_cli],
                    "SELECT extension_name, loaded, extension_version FROM duckdb_extensions() WHERE extension_name='test_utils';",
                )

            if not has_checked_versions:
                versions = get_extensions_versions([old_cli, new_cli])
                if not same_extension_version(versions[0], versions[1]):
                    # make release EXTENSION_CONFIGS=.github/config/extensions/test-utils.cmake to compile in this repo
                    raise RuntimeError(
                        f"test-utils extension version mismatch: {old_cli.version} is @ {versions[0]} and {new_cli.version} is @ {versions[1]}"
                    )
                has_checked_versions = True

            # Cleanup output directory in case previous runs left files there
            test_spec.reset_output_directory()

            # First serialize the queries & run them in the first version
            compare_func = "tu_compare_results_from_memory"
            compare_args = [test_spec.results_new_file_name]
            if test_spec.has_cached_serialized_plans:
                compare_args.append(test_spec.results_old_file_name)  # Load old results for comparison
                compare_func = "tu_compare_results_from_file"
                report.end_cached_step(
                    "serialize_queries_plans", test_spec.queries_file_name, test_spec.serialized_plans_file_name
                )
                report.end_cached_step("serialize_results", test_spec.results_old_file_name)
            else:
                if not run_step(
                    old_cli,
                    report,
                    "serialize_queries_plans",
                    test_spec.queries_file_name,
                    test_spec.serialized_plans_file_name,
                ):
                    return report

                # TODO - merge results in serialized file?
                if not run_step(old_cli, report, "serialize_results", test_spec.results_old_file_name):
                    return report

                # We need to provide a clean output folder for the other CLI
                test_spec.reset_output_directory()

            # Then execute the plans in the second version
            if not run_step(
                new_cli,
                report,
                "execute_all_plans_from_file",
                test_spec.serialized_plans_file_name,
                test_spec.results_new_file_name,
            ):
                return report

            # Now compare the results
            run_step(old_cli, report, compare_func, *compare_args)
            report.load_comparison_results()
            return report


def write_summary_file(ctx, to_write):
    with ctx.report_lock:
        ctx.summary_file.write(to_write)
        ctx.summary_file.flush()


def run_one_test_and_log(ctx, test):
    if cancel_event.is_set():
        return None

    try:
        report = do_run_test(ctx, test)
        if report.is_successful():
            write_summary_file(ctx, f"{test.test_spec_relative_path}\n")
            logger.info(f"Test {test.test_idx}/{ctx.nb_tests}: {test.test_spec_relative_path} - SUCCESS")
        else:
            em = report.find_exception_message()
            write_summary_file(ctx, f"{test.test_spec_relative_path} # FAILED: {em}\n")
            logger.error(f"Test {test.test_idx}/{ctx.nb_tests}: {test.test_spec_relative_path} - ❌ FAILED: {em}")

        with ctx.report_lock:
            ctx.report_con.execute(TestReport.report_insert(), report.report_sql_values())
            ctx.report_con.commit()

        return report
    except Exception as e:
        cancel_event.set()
        raise


def cleanup_runtime_dir(bwc_tests_base_dir, dry_run=True):
    runtime_dir = f"{bwc_tests_base_dir}/runtime"
    logger.info(f"Cleaning up BWC directory '{runtime_dir}'")
    delete_list = []
    # Remove new result files, any 'output' dir, and 'data' & 'test' symlinks
    for root, dirs, files in os.walk(runtime_dir):
        for file in files:
            file_path = os.path.join(root, file)
            if file.endswith(".new.result.bin"):
                delete_list.append(file_path)
        for dir in dirs:
            dir_path = os.path.join(root, dir)
            if dir.startswith("output") or (os.path.islink(dir_path) and (dir == "data" or dir == "test")):
                delete_list.append(dir_path)

    if dry_run:
        logger.info(
            f"{len(delete_list)} files would be deleted - re-run with `--dry_run=False` argument to actually delete them"
        )
        for file_path in delete_list:
            logger.debug(f"  {file_path}")
    else:
        removed = 0
        for file_path in delete_list:
            try:
                if os.path.isfile(file_path) or os.path.islink(file_path):
                    os.remove(file_path)
                    removed += 1
                elif os.path.isdir(file_path):
                    shutil.rmtree(file_path)
                    removed += 1
            except Exception as e:
                logger.error(f"Failed to delete '{file_path}': {e}")
        logger.info(f"Cleanup completed, deleted {removed}/{len(delete_list)} files/directories")


if __name__ == "__main__":
    supported_duckdb_versions = (
        [args.old_duckdb_version] if args.old_duckdb_version else list_supported_duckdb_versions()
    )

    duckdb_root_dir = dirname(dirname(dirname(abspath(__file__))))
    logger.info(f"DuckDB root dir: '{duckdb_root_dir}'")
    bwc_tests_base_dir = f"{duckdb_root_dir}/duckdb_unittest_tempdir/bwc"

    if args.cleanup_bwc_directory:
        cleanup_runtime_dir(bwc_tests_base_dir, dry_run=args.dry_run)
        sys.exit(0)

    # Create the report database
    os.makedirs(f"{bwc_tests_base_dir}/reports", exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    version_suffix = args.old_duckdb_version if args.old_duckdb_version else "multi_version"
    report_db_path = f"{bwc_tests_base_dir}/reports/test_report_{version_suffix}_{ts}.duckdb"
    con = duckdb.connect(report_db_path)
    con.execute(TestReport.report_schema())
    con.commit()

    logger.info(f"Report database created at '{report_db_path}'")

    # Install DuckDB CLIs and test suites for all supported versions
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(lambda version: install_assets(version, bwc_tests_base_dir), supported_duckdb_versions))

    nb_tests_run = 0
    nb_success = 0
    failed_tests = []
    for old_duckdb_version in supported_duckdb_versions:
        logger.info(f"Running tests for DuckDB version '{old_duckdb_version}'")

        new_cli_path = f"{duckdb_root_dir}/build/release/duckdb" if args.new_cli_path is None else args.new_cli_path

        summary_file_path = f"{bwc_tests_base_dir}/tests_summary_{old_duckdb_version}.txt"
        with open(summary_file_path, 'a') as summary_file:
            summary_file.write(f"\n## --- Starting run at {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
            summary_file.flush()
            test_pattern = args.test_pattern
            single_test_file = args.test_file
            res = load_test_files(
                duckdb_root_dir, bwc_tests_base_dir, old_duckdb_version, test_pattern, single_test_file
            )
            tests = res['tests']

            runner_context = TestRunnerContext(old_duckdb_version, new_cli_path, len(tests), summary_file, con)
            runner_context.initialize()

            extensions = res['needed_extensions']
            extensions.add('test_utils')
            install_extensions(runner_context.old_cli_path, extensions)
            install_extensions(runner_context.new_cli_path, extensions)

            run_sequentially = args.run_sequentially or test_pattern is not None
            stop_on_failure = args.stop_on_failure
            start_time = time.monotonic()
            nb_tests_run = 0
            if run_sequentially:
                for test in tests:
                    report = run_one_test_and_log(runner_context, test)
                    nb_tests_run += 1

                    if report.is_successful():
                        nb_success += 1
                    else:
                        failed_tests.append((old_duckdb_version, report.test_relative_path))
                        if stop_on_failure:
                            break
                        elif test_pattern is not None:
                            report.log_errors()
                            report.log_steps_queries()
            else:
                with ThreadPoolExecutor(max_workers=args.max_workers) as executor:
                    reports = executor.map(lambda test: run_one_test_and_log(runner_context, test), tests)

                reports_list = list(reports)
                nb_tests_run = len(reports_list)
                nb_success = sum(1 for r in reports_list if r.is_successful())
                for r in reports_list:
                    if not r.is_successful():
                        failed_tests.append((old_duckdb_version, r.test_relative_path))

            elapsed = time.monotonic() - start_time
            tps = nb_tests_run / elapsed if elapsed > 0 else 0
            nb_failed = nb_tests_run - nb_success
            logger.info(
                f"All tests completed in {elapsed:.2f}s - {nb_tests_run} tests run, {nb_success} successful, {nb_failed} failed - {tps:.2f} tests per second."
            )

    if len(failed_tests) == 0:
        logger.info("All tests passed successfully! 🎉")
        sys.exit(0)

    last_version = None
    for version, test_path in failed_tests:
        if version != last_version:
            logger.info(f"Failed tests for DuckDB version '{version}':")
            logger.info(f"{'-'*40}")
            last_version = version
        logger.info(test_path)
    sys.exit(1)
