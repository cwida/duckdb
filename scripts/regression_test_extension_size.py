import os
import argparse
from pathlib import Path

# the threshold at which we consider something a regression (percentage)
regression_threshold_percentage = 0.20

parser = argparse.ArgumentParser(description='Check DuckDB binary sizes for regressions.')
parser.add_argument('--old', dest='old_dir', action='store', help='Path to the old release dir', required=True)
parser.add_argument('--new', dest='new_dir', action='store', help='Path to the new release dir', required=True)
parser.add_argument(
    '--expect',
    dest='expected_artifacts_raw',
    action='store',
    help='Comma separated list of expected artifacts',
    required=True,
)

args = parser.parse_args()


expected_artifacts = args.expected_artifacts_raw.split(',')


def parse_artifacts(directory):
    result = {}

    cli_path = Path(directory, 'duckdb')
    if cli_path.is_file():
        result['cli'] = cli_path

    extension_dir = Path(directory, 'repository')
    if not extension_dir.is_dir():
        extension_dir = Path(directory)

    for root, dirs, files in os.walk(extension_dir):
        for filename in files:
            if filename.endswith(".duckdb_extension"):
                result[Path(filename).stem] = Path(root, filename)

    # Check all expected artifacts are there
    for expected_artifact in expected_artifacts:
        if expected_artifact not in result:
            print(f"Did not find expected artifact {expected_artifact} in {directory}")
            exit(1)

    return result


old_artifacts = parse_artifacts(args.old_dir)
new_artifacts = parse_artifacts(args.new_dir)

matching_artifacts = []

for artifact in old_artifacts.keys():
    if artifact in new_artifacts:
        matching_artifacts.append(artifact)

check_passed = True
error_message = ""

for artifact in matching_artifacts:
    old_size = os.path.getsize(old_artifacts[artifact])
    new_size = os.path.getsize(new_artifacts[artifact])

    print(f" - checking '{artifact}': old size={old_size}, new_size={new_size}")

    if new_size / (old_size + 0.1) > (1.0 + regression_threshold_percentage):
        check_passed = False
        error_message += f" - Artifact '{artifact}' was bigger than expected {new_size}\n"
        error_message += f"   - old size: {old_size}\n"
        error_message += f"   - new size: {new_size}\n"

print()
if not check_passed:
    print("Binary size regression check failed:\n")
    print(error_message)
    exit(1)
else:
    print("All artifacts passed the check!")
