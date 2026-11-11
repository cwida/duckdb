import os
import argparse
import subprocess
import shutil

parser = argparse.ArgumentParser(
    description='''Runs storage tests both with explicit one-initialization and with explicit zero-initialization, and verifies that the final storage files are the same.
The purpose of this is to verify all memory is correctly initialized before writing to disk - which prevents leaking of in-memory data in storage files by writing uninitialized memory to disk.'''
)
parser.add_argument('--unittest', default='build/debug/test/unittest', help='path to unittest', dest='unittest')
parser.add_argument(
    '--zero_init_dir',
    default='test_zero_init_db',
    help='directory to write zero-initialized databases to',
    dest='zero_init_dir',
)
parser.add_argument(
    '--standard_dir', default='test_standard_db', help='directory to write regular databases to', dest='standard_dir'
)

args = parser.parse_args()

test_list = [
    'test/sql/index/art/storage/test_art_checkpoint.test',
    'test/sql/storage/compression/simple_compression.test',
    'test/sql/storage/compression/compression_padding_initialization.test',
    'test/sql/storage/compression/uncompressed/string_gap_reused_memory.test',
    'test/sql/index/art/storage/art_reloaded_buffer_initialization.test',
    'test/sql/storage/delete/test_store_deletes.test',
    'test/sql/storage/mix/test_update_delete_string.test',
    'test/sql/storage/nested/struct_of_lists_unaligned.test',
    'test/sql/storage/test_store_integers.test',
    'test/sql/storage/test_store_nulls_strings.test',
    'test/sql/storage/update/test_store_null_updates.test',
]

# tests that write non-deterministic files, which check for unwritten bytes themselves when run with --one-initialize
one_initialize_test_list = [
    'test/sql/storage/compression/fsst/fsst_uncompacted_gap_initialization.test',
]


def run_test(args):
    res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout = res.stdout.decode('utf8').strip()
    stderr = res.stderr.decode('utf8').strip()
    if res.returncode != 0:
        print("Failed to run test!")
        print("----------COMMAND-----------")
        print(' '.join(args))
        print("----------STDOUT-----------")
        print(stdout)
        print("----------STDERR-----------")
        print(stderr)
        print("---------------------")
        exit(1)


header_size = 4096 * 3
default_block_alloc_size = 262144
checksum_size = 8
# the main header contains a random database identifier, which also changes the main header checksum
random_header_ranges = [(0, checksum_size), (124, 140)]
block_alloc_size_offset = 4096 + checksum_size + 4 * 8


def is_random_header_byte(i):
    return any(start <= i < end for start, end in random_header_ranges)


def get_block_alloc_size(data):
    if len(data) < block_alloc_size_offset + 8:
        return default_block_alloc_size
    block_alloc_size = int.from_bytes(data[block_alloc_size_offset : block_alloc_size_offset + 8], 'little')
    return block_alloc_size if block_alloc_size > 0 else default_block_alloc_size


def handle_error(i, block_alloc_size, standard_db, zero_init_db, standard_data, zero_data):
    print("------------------------------------------------------------------")
    print(f"FAIL - Mismatch between one-initialized and zero-initialized databases at byte position {i}")
    print("------------------------------------------------------------------")
    print(f"One-initialized database {standard_db} - byte value {standard_data}")
    print(f"Zero-initialized database {zero_init_db} - byte value {zero_data}")
    if i < header_size:
        print("This byte is in the initial headers of the file")
    else:
        byte_pos = (i - header_size) % block_alloc_size
        block_id = (i - header_size) // block_alloc_size
        if byte_pos >= checksum_size:
            print(
                f"This byte is in block id {block_id} at byte position {byte_pos - checksum_size} (position {byte_pos} including the block checksum)"
            )
        else:
            print(f"This byte is in block id {block_id} at byte position {byte_pos}")
            print("This is in the checksum part of the block")
    print("------------------------------------------------------------------")
    print(
        "This error likely means that memory was not correctly zero-initialized in a block before being written out to disk."
    )


def compare_database(standard_db, zero_init_db):
    with open(standard_db, 'rb') as f:
        standard_data = f.read()
    with open(zero_init_db, 'rb') as f:
        zero_data = f.read()
    if len(standard_data) != len(zero_data):
        print(
            f"FAIL - Length mismatch between database {standard_db} ({str(len(standard_data))}) and {zero_init_db} ({str(len(zero_data))})"
        )
        return False
    block_alloc_size = get_block_alloc_size(zero_data)
    checksum_error = None
    for i in range(len(standard_data)):
        if standard_data[i] == zero_data[i] or is_random_header_byte(i):
            continue
        if i >= header_size and (i - header_size) % block_alloc_size < checksum_size:
            # a differing block checksum only tells us the block payload differs, report the payload first
            if checksum_error is None:
                checksum_error = i
            continue
        handle_error(i, block_alloc_size, standard_db, zero_init_db, standard_data[i], zero_data[i])
        return False
    if checksum_error is not None:
        i = checksum_error
        handle_error(i, block_alloc_size, standard_db, zero_init_db, standard_data[i], zero_data[i])
        return False
    print("Success!")
    return True


def compare_files(standard_dir, zero_init_dir):
    standard_list = os.listdir(standard_dir)
    zero_init_list = os.listdir(zero_init_dir)
    standard_list.sort()
    zero_init_list.sort()
    if standard_list != zero_init_list:
        print(
            f"FAIL - Directories contain mismatching files (standard - {str(standard_list)}, zero init - {str(zero_init_list)})"
        )
        return False
    if len(standard_list) == 0:
        print("FAIL - Directory is empty!")
        return False
    success = True
    for entry in standard_list:
        if not compare_database(os.path.join(standard_dir, entry), os.path.join(zero_init_dir, entry)):
            success = False
    return success


def clear_directories(directories):
    for dir in directories:
        try:
            shutil.rmtree(dir)
        except FileNotFoundError as e:
            pass


test_dirs = [args.standard_dir, args.zero_init_dir]

success = True
for test in test_list:
    print(f"Running test {test}")
    clear_directories(test_dirs)
    # TEMP_DIR must BE the given dir exactly and survive for the post-run byte diff below,
    # so pin the root with both levels off and destroy=never (the dirs are pre-rmtree'd
    # above, so the default create=on-absent is fine). --database-destroy off keeps the
    # generated DB files (otherwise the harness deletes loaded DBs under TEMP_DIR, leaving
    # the retained dir empty and the compare with nothing to diff).
    temp_dir_flags = lambda d: [
        '--temp-dir-root',
        d,
        '--temp-dir-run-id',
        'off',
        '--temp-dir-test-id',
        'off',
        '--temp-dir-destroy',
        'never',
        '--database-destroy',
        'off',
    ]
    standard_args = [args.unittest, *temp_dir_flags(args.standard_dir), '--one-initialize', '--single-threaded', test]
    zero_init_args = [
        args.unittest,
        *temp_dir_flags(args.zero_init_dir),
        '--zero-initialize',
        '--single-threaded',
        test,
    ]
    print(f"Running test in one-initialize mode")
    run_test(standard_args)
    print(f"Running test in zero-initialize mode")
    run_test(zero_init_args)
    if not compare_files(args.standard_dir, args.zero_init_dir):
        success = False

for test in one_initialize_test_list:
    print(f"Running test {test} in one-initialize mode")
    run_test([args.unittest, '--one-initialize', '--single-threaded', test])

clear_directories(test_dirs)

if not success:
    exit(1)
