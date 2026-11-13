#!/usr/bin/python

# this script is used to format the source directory

import os
import time
import sys
import inspect
import subprocess
import difflib
import re
import tempfile
import uuid
import concurrent.futures
import argparse
import shutil
import traceback
from python_helpers import open_utf8

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from format_test_benchmark import format_file_content

# Ensure binaries installed into the current Python environment are discoverable.
# This is required when invoking this script via an explicit venv python path.
python_bin_dir = os.path.dirname(os.path.abspath(sys.executable))
os.environ['PATH'] = python_bin_dir + os.pathsep + os.environ.get('PATH', '')

try:
    ver = subprocess.check_output(('black', '--version'), text=True)
    if int(ver.split(' ')[1].split('.')[0]) < 24:
        print('you need to run `pip install "black>=24"`', ver)
        if 'DUCKDB_FORMAT_SKIP_VERSION_CHECKS' not in os.environ:
            exit(-1)
except Exception as e:
    print('you need to run `pip install "black>=24"`', e)
    exit(-1)

try:
    ver = subprocess.check_output(('clang-format', '--version'), text=True)
    if '11.' not in ver:
        print('you need to run `pip install clang_format==11.0.1 - `', ver)
        if 'DUCKDB_FORMAT_SKIP_VERSION_CHECKS' not in os.environ:
            exit(-1)
except Exception as e:
    print('you need to run `pip install clang_format==11.0.1 - `', e)
    exit(-1)

cpp_format_command = 'clang-format --sort-includes=0 -style=file'
cmake_format_command = 'cmake-format'

try:
    subprocess.check_output(('cmake-format', '--version'), text=True)
except Exception as e:
    print('you need to run `pip install cmake-format`', e)
    exit(-1)

extensions = [
    '.cpp',
    '.ipp',
    '.c',
    '.hpp',
    '.h',
    '.cc',
    '.hh',
    'CMakeLists.txt',
    '.test',
    '.test_slow',
    '.test_coverage',
    '.benchmark',
    '.py',
    '.java',
]
formatted_directories = ['src', 'benchmark', 'test', 'tools', 'examples', 'extension', 'scripts']
ignored_files = [
    'tpch_constants.hpp',
    'tpcds_constants.hpp',
    '_generated',
    'test_csv_header.hpp',
    'duckdb.cpp',
    'duckdb.hpp',
    'json.hpp',
    'sqlite3.h',
    'shell.c',
    'termcolor.hpp',
    'test_insert_invalid.test',
    'httplib.hpp',
    'os_win.c',
    'glob.c',
    'printf.c',
    'helper.hpp',
    'single_thread_ptr.hpp',
    'types.hpp',
    'default_views.cpp',
    'default_functions.cpp',
    'release.h',
    'genrand.cpp',
    'address.cpp',
    'visualizer_constants.hpp',
    'icu-collate.cpp',
    'icu-collate.hpp',
    'yyjson.cpp',
    'yyjson.hpp',
    'duckdb_pdqsort.hpp',
    'pdqsort.h',
    'stubdata.cpp',
    'nf_calendar.cpp',
    'nf_calendar.h',
    'nf_localedata.cpp',
    'nf_localedata.h',
    'nf_zformat.cpp',
    'nf_zformat.h',
    'expr.cc',
    'function_list.cpp',
    'inlined_grammar.hpp',
]
ignored_directories = [
    '.eggs',
    '__pycache__',
    'dbgen',
    os.path.join('tools', 'rpkg', 'src', 'duckdb'),
    os.path.join('tools', 'rpkg', 'inst', 'include', 'cpp11'),
    os.path.join('extension', 'external'),
    os.path.join('extension', 'tpcds', 'dsdgen'),
    os.path.join('extension', 'icu', 'third_party'),
    os.path.join('extension', 'icu', 'datetime', 'generated'),
    os.path.join('tools', 'nodejs', 'src', 'duckdb'),
]
format_all = False
check_only = True
confirm = True
silent = False
force = False
typos_files = None


parser = argparse.ArgumentParser(prog='python scripts/format.py', description='Format source directory files')
parser.add_argument(
    'revision', nargs='?', default='HEAD', help='Revision number or --all to format all files (default: HEAD)'
)
parser.add_argument('--check', action='store_true', help='Only print differences (default)')
parser.add_argument('--fix', action='store_true', help='Fix the files')
parser.add_argument('-a', '--all', action='store_true', help='Format all files')
parser.add_argument('-d', '--directories', nargs='*', default=[], help='Format specified directories')
parser.add_argument('-C', '--workdir', type=str, help='Change work directory')
parser.add_argument('-y', '--noconfirm', action='store_true', help='Skip confirmation prompt')
parser.add_argument('-q', '--silent', action='store_true', help='Suppress output')
parser.add_argument('-f', '--force', action='store_true', help='Force formatting')
parser.add_argument(
    '--staged',
    action='store_true',
    help='Format the files staged for commit instead of the files changed since a revision',
)
parser.add_argument(
    '--modified-list',
    type=str,
    help='Write the NUL-separated paths of the files that were actually rewritten to this file',
)
parser.add_argument('--files-from', type=str, help='Read newline-separated paths to format from this file')
parser.add_argument(
    '--skip-typos',
    action='store_true',
    default=os.environ.get('DUCKDB_FORMAT_SKIP_TYPOS') == '1',
    help='Skip the typos check',
)
parser.add_argument(
    '-j',
    '--jobs',
    type=int,
    default=int(os.environ.get('DUCKDB_FORMAT_JOBS', min(32, (os.cpu_count() or 1) + 4))),
    help='Number of files to format concurrently',
)
args = parser.parse_args()

revision = args.revision
if args.check and args.fix:
    parser.print_usage()
    exit(1)
if args.jobs < 1:
    parser.error('--jobs must be at least 1')
if args.files_from and (args.all or args.staged or args.directories):
    parser.error('--files-from cannot be combined with --all, --staged, or --directories')

if args.workdir:
    os.chdir(args.workdir)

check_only = not args.fix
confirm = not args.noconfirm
silent = args.silent
force = args.force
format_all = args.all
if args.directories:
    formatted_directories = args.directories


def get_typos_targets():
    if typos_files is not None:
        return typos_files
    if format_all:
        return [path for path in formatted_directories if os.path.exists(path)]
    return sorted(set([f.full_path for f in files if os.path.exists(f.full_path)]))


def run_typos_check():
    if args.skip_typos:
        return 0
    typos_targets = get_typos_targets()
    if not typos_targets:
        return 0
    # Ignore typos check for non-duckdb code.
    if not os.path.isfile('scripts/typos.toml'):
        return 0
    typos_command = ['typos', '--force-exclude']
    if not check_only:
        typos_command.append('-w')
    typos_command += ['-c', 'scripts/typos.toml']
    # check the files in chunks to bound the memory usage of a single typos process
    typos_chunk_size = 500
    status = 0
    try:
        for start in range(0, len(typos_targets), typos_chunk_size):
            chunk_status = subprocess.call(typos_command + typos_targets[start : start + typos_chunk_size])
            status = status or chunk_status
    except FileNotFoundError:
        print('typos not found. Install it with "brew install typos-cli"')
        return 1
    return status


def file_is_ignored(full_path):
    if os.path.basename(full_path) in ignored_files:
        return True
    dirnames = os.path.sep.join(full_path.split(os.path.sep)[:-1])
    for ignored_directory in ignored_directories:
        if ignored_directory in dirnames:
            return True
    return False


def can_format_file(full_path):
    global extensions, formatted_directories, ignored_files
    if not os.path.isfile(full_path):
        return False
    fname = full_path.split(os.path.sep)[-1]
    found = False
    # check file extension
    for ext in extensions:
        if full_path.endswith(ext):
            found = True
            break
    if not found:
        return False
    # check ignored files
    if file_is_ignored(full_path):
        return False
    # the repository root is not covered by formatted_directories
    if full_path == 'CMakeLists.txt':
        return True
    # now check file directory
    for dname in formatted_directories:
        if full_path.startswith(dname):
            return True
    return False


action = "Formatting"
if check_only:
    action = "Checking"


def get_changed_files(revision, staged=False):
    command = ['git', 'diff', '--name-only']
    if staged:
        command.append('--cached')
    command.append(revision)
    proc = subprocess.Popen(command, stdout=subprocess.PIPE)
    files = proc.stdout.read().decode('utf8', errors='backslashreplace').split('\n')
    changed_files = []
    for f in files:
        if not can_format_file(f):
            continue
        if file_is_ignored(f):
            continue
        changed_files.append(f)
    return changed_files


if args.files_from:
    source_name = "standard input" if args.files_from == "-" else args.files_from
    print(action + " files listed in " + source_name)
    if args.files_from == "-":
        listed_files = [line.rstrip('\r\n') for line in sys.stdin]
    else:
        with open(args.files_from, encoding='utf8', errors='surrogateescape') as source:
            listed_files = [line.rstrip('\r\n') for line in source]
    listed_files = list(dict.fromkeys(listed_files))
    typos_files = sorted(set(path for path in listed_files if os.path.exists(path)))
    changed_files = [path for path in listed_files if can_format_file(path)]
    if len(changed_files) == 0:
        print("No files to format!")
elif args.staged:
    print(action + " files staged for commit")
    changed_files = get_changed_files(revision, staged=True)
    if len(changed_files) == 0:
        print("No staged files found!")
        exit(0)

    print("Changeset:")
    for fname in changed_files:
        print(fname)
elif os.path.isfile(revision):
    print(action + " individual file: " + revision)
    changed_files = [revision]
elif os.path.isdir(revision):
    print(action + " files in directory: " + revision)
    changed_files = [os.path.join(revision, x) for x in os.listdir(revision)]

    print("Changeset:")
    for fname in changed_files:
        print(fname)
elif not format_all:
    if revision == 'main':
        # fetch new changes when comparing to the master
        os.system("git fetch origin main:main")
    print(action + " since branch or revision: " + revision)
    changed_files = get_changed_files(revision)
    if len(changed_files) == 0:
        print("No changed files found!")
        exit(0)

    print("Changeset:")
    for fname in changed_files:
        print(fname)
else:
    print(action + " all files")

if confirm and not check_only:
    print("The files listed above will be reformatted.")
    result = input("Continue with changes (y/n)?\n")
    if result != 'y':
        print("Aborting.")
        exit(0)

format_commands = {
    '.cpp': cpp_format_command,
    '.ipp': cpp_format_command,
    '.c': cpp_format_command,
    '.hpp': cpp_format_command,
    '.h': cpp_format_command,
    '.hh': cpp_format_command,
    '.cc': cpp_format_command,
    '.txt': cmake_format_command,
    '.py': 'black --quiet - --skip-string-normalization --line-length 120 --stdin-filename',
    '.java': cpp_format_command,
}

difference_files = []
modified_files = []
failed_files = []

header_top = "//===----------------------------------------------------------------------===//\n"
header_top += "//                         DuckDB\n" + "//\n"
header_bottom = "//\n" + "//\n"
header_bottom += "//===----------------------------------------------------------------------===//\n\n"
base_dir = os.path.join(os.getcwd(), 'src/include')


def get_formatted_text(f, full_path, directory, ext):
    if not can_format_file(full_path):
        if not force:
            print(
                "File "
                + full_path
                + " is not normally formatted - but attempted to format anyway. Use --force if formatting is desirable"
            )
            exit(1)
    if f == 'list.hpp':
        # fill in list file
        file_list = [
            os.path.join(dp, f)
            for dp, dn, filenames in os.walk(directory)
            for f in filenames
            if os.path.splitext(f)[1] == '.hpp' and not f.endswith("list.hpp")
        ]
        file_list = [x.replace('src/include/', '') for x in file_list]
        file_list.sort()
        result = ""
        for x in file_list:
            result += '#include "%s"\n' % (x)
        return result

    if ext == ".hpp" and directory.startswith("src/include"):
        with open_utf8(full_path, 'r') as f:
            lines = f.readlines()

        # format header in files
        header_middle = "// " + os.path.relpath(full_path, base_dir) + "\n"
        text = header_top + header_middle + header_bottom
        is_old_header = True
        for line in lines:
            if not (line.startswith("//") or line.startswith("\n")) and is_old_header:
                is_old_header = False
            if not is_old_header:
                text += line

    if ext == '.test' or ext == '.test_slow' or ext == '.test_coverage' or ext == '.benchmark':
        # optimization: import and call the function directly
        # instead of running a subprocess
        with open(full_path, "r", encoding="utf-8", errors='surrogateescape') as f:
            original_lines = f.readlines()
        formatted, status = format_file_content(full_path, original_lines)
        if formatted is None:
            print(f"Failed to format {full_path}: {status}")
            sys.exit(1)
        return formatted
    proc_command = format_commands[ext].split(' ') + [full_path]
    proc = subprocess.Popen(
        proc_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        stdin=open(full_path, encoding='utf8', errors='backslashreplace') if ext == '.py' else None,
    )
    new_text = proc.stdout.read().decode('utf8', errors='backslashreplace')
    stderr = proc.stderr.read().decode('utf8', errors='backslashreplace')
    if len(stderr) > 0:
        print(os.getcwd())
        print("Failed to format file " + full_path)
        print(' '.join(proc_command))
        print(stderr)
        exit(1)
    new_text = new_text.replace('\r', '')
    new_text = re.sub(r'\n*$', '', new_text)
    return new_text + '\n'


def file_is_generated(text):
    if '// This file is automatically generated by scripts/' in text:
        return True
    return False


def format_file(f, full_path, directory, ext):
    global difference_files, modified_files
    with open_utf8(full_path, 'r', errors='surrogateescape') as f:
        old_text = f.read()
    # do not format auto-generated files
    if file_is_generated(old_text) and ext != '.py':
        return
    old_lines = old_text.split('\n')

    new_text = get_formatted_text(f, full_path, directory, ext)
    if ext in ('.cpp', '.hpp'):
        new_text = new_text.replace('ARGS &&...args', 'ARGS &&... args')
    if check_only:
        new_lines = new_text.split('\n')
        old_lines = [x for x in old_lines if '...' not in x]
        new_lines = [x for x in new_lines if '...' not in x]
        diff_result = difflib.unified_diff(old_lines, new_lines)
        total_diff = ""
        for diff_line in diff_result:
            total_diff += diff_line + "\n"
        total_diff = total_diff.strip()

        if len(total_diff) > 0:
            print("----------------------------------------")
            print("----------------------------------------")
            print("Found differences in file " + full_path)
            print("----------------------------------------")
            print("----------------------------------------")
            print(total_diff)
            difference_files.append(full_path)
    else:
        if new_text == old_text:
            return
        tmpfile = os.path.join(tempfile.gettempdir(), str(uuid.uuid4()))
        with open_utf8(tmpfile, 'w+', newline='\n', errors='surrogateescape') as f:
            f.write(new_text)
        shutil.move(tmpfile, full_path)
        modified_files.append(full_path)


class ToFormatFile:
    def __init__(self, filename, full_path, directory):
        self.filename = filename
        self.full_path = full_path
        self.directory = directory
        self.ext = '.' + filename.split('.')[-1]


def format_directory(directory):
    files = os.listdir(directory)
    files.sort()
    result = []
    for f in files:
        full_path = os.path.join(directory, f)
        if os.path.isdir(full_path):
            if f in ignored_directories or full_path in ignored_directories:
                continue
            result += format_directory(full_path)
        elif can_format_file(full_path):
            result += [ToFormatFile(f, full_path, directory)]
    return result


files = []
if format_all:
    files.append(ToFormatFile('CMakeLists.txt', 'CMakeLists.txt', '.'))
    for direct in formatted_directories:
        files += format_directory(direct)

else:
    for full_path in changed_files:
        splits = full_path.split(os.path.sep)
        fname = splits[-1]
        dirname = os.path.sep.join(splits[:-1])
        files.append(ToFormatFile(fname, full_path, dirname))


def process_file(f):
    if not silent:
        print(f.full_path)
    try:
        format_file(f.filename, f.full_path, f.directory, f.ext)
    except:
        print(traceback.format_exc())
        # sys.exit() in a worker thread only kills that thread, so the failure is
        # collected here and turned into a non-zero exit code by the main thread
        failed_files.append(f.full_path)


# Create thread for each file
with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
    try:
        threads = [executor.submit(process_file, f) for f in files]
        # Wait for all tasks to complete
        concurrent.futures.wait(threads)
    except KeyboardInterrupt:
        executor.shutdown(wait=True, cancel_futures=True)
        raise

if args.modified_list:
    with open(args.modified_list, 'wb') as f:
        for fname in modified_files:
            f.write(fname.encode('utf8', errors='surrogateescape') + b'\0')

if failed_files:
    print("Failed to format the following files:")
    for fname in failed_files:
        print("- " + fname)
    exit(1)

typos_status = run_typos_check()

if check_only:
    if len(difference_files) > 0:
        print("")
        print("")
        print("")
        print("Failed format-check: differences were found in the following files:")
        for fname in difference_files:
            print("- " + fname)
        print('Run "make format-fix" to fix these differences automatically')
        exit(1)
    if typos_status != 0:
        exit(1)
    else:
        print("Passed format-check")
        exit(0)
elif typos_status != 0:
    exit(1)
