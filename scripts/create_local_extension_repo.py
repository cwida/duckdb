###
# This script copies all extensions in a build folder from their cmake-produced structure into the extension repository
# structure of ./<duckdb_version>/<build_architecture>/<extension_name>.duckdb_extension
# Note that it requires duckdb_platofrom_out file to be populated with the platform

import os
import sys
import subprocess
import glob
import shutil

if len(sys.argv) != 6:
    print(
        "Usage: scripts/create_local_extension_repo.py <duckdb_version> <duckdb_platform_out> <path/to/duckdb/build> <path/to/local_repo> <postfix>"
    )
    exit(1)

duckdb_version = sys.argv[1]
duckdb_platform_out = sys.argv[2]
extension_path = sys.argv[3]
dst_path = sys.argv[4]
postfix = sys.argv[5]

if os.name == 'nt':
    duckdb_platform_out = duckdb_platform_out.replace("/", "\\")
    extension_path = extension_path.replace("/", "\\")
    dst_path = dst_path.replace("/", "\\")

with open(duckdb_platform_out, 'r') as f:
    lines = f.readlines()
    duckdb_platform = lines[0]

# Create destination path
dest_path = os.path.join(dst_path, duckdb_version, duckdb_platform)
if not os.path.exists(dest_path):
    os.makedirs(dest_path)

# Now copy over the extensions to the correct path
glob_string = os.path.join(extension_path, 'extension', '*', '*.' + postfix)

for file in glob.glob(glob_string):
    dest_file = os.path.join(dest_path, os.path.basename(file))
    # This runs on every build, so skip extensions that weren't rebuilt instead of copying hundreds of MBs each time.
    # copy2 keeps the mtime, which is what makes this check work.
    if os.path.exists(dest_file):
        src_stat = os.stat(file)
        dest_stat = os.stat(dest_file)
        if src_stat.st_size == dest_stat.st_size and src_stat.st_mtime_ns == dest_stat.st_mtime_ns:
            continue
    shutil.copy2(file, dest_file)
