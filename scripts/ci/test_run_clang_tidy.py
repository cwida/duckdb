#!/usr/bin/env python3

import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_PATH = REPO_ROOT / "scripts" / "run-clang-tidy.py"
SPEC = importlib.util.spec_from_file_location("run_clang_tidy", RUNNER_PATH)
RUN_CLANG_TIDY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN_CLANG_TIDY)


class RunClangTidyTest(unittest.TestCase):
    def test_shards_are_deterministic_disjoint_and_complete(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            files = [source_root / "src" / f"file_{index}.cpp" for index in range(100)]
            third_party_path = source_root / "third_party"
            file_name_re = re.compile(".*")

            shards = []
            for shard_index in range(4):
                selected, eligible_count = RUN_CLANG_TIDY.select_files_for_shard(
                    [str(path) for path in files],
                    str(source_root),
                    str(third_party_path),
                    file_name_re,
                    shard_index,
                    4,
                )
                self.assertEqual(eligible_count, len(files))
                self.assertEqual(
                    selected,
                    RUN_CLANG_TIDY.select_files_for_shard(
                        [str(path) for path in files],
                        str(source_root),
                        str(third_party_path),
                        file_name_re,
                        shard_index,
                        4,
                    )[0],
                )
                shards.append(set(selected))

            for left_index, left_shard in enumerate(shards):
                for right_shard in shards[left_index + 1 :]:
                    self.assertFalse(left_shard & right_shard)
            self.assertEqual(set().union(*shards), {str(path) for path in files})

    def test_filters_third_party_and_non_matching_files_before_sharding(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            files = [
                source_root / "src" / "included.cpp",
                source_root / "src" / "excluded.c",
                source_root / "third_party" / "dependency.cpp",
            ]

            selected, eligible_count = RUN_CLANG_TIDY.select_files_for_shard(
                [str(path) for path in files],
                str(source_root),
                str(source_root / "third_party"),
                re.compile(r"\.cpp$"),
                0,
                1,
            )

            self.assertEqual(selected, [str(files[0])])
            self.assertEqual(eligible_count, 1)

    def test_validates_shard_bounds(self):
        RUN_CLANG_TIDY.validate_shard(0, 1)
        RUN_CLANG_TIDY.validate_shard(3, 4)
        with self.assertRaisesRegex(ValueError, "at least 1"):
            RUN_CLANG_TIDY.validate_shard(0, 0)
        with self.assertRaisesRegex(ValueError, "between 0"):
            RUN_CLANG_TIDY.validate_shard(-1, 4)
        with self.assertRaisesRegex(ValueError, "between 0"):
            RUN_CLANG_TIDY.validate_shard(4, 4)


if __name__ == "__main__":
    unittest.main()
