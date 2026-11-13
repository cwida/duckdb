#!/usr/bin/env python3
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "requires CMake and Ninja")
class ExtensionLinkageTest(unittest.TestCase):
    def check_linkage(self, static_build):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            query = build / ".cmake/api/v1/query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(REPO_ROOT),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DBUILD_EXTENSIONS=tpch",
                    "-DSTATICALLY_LINK_EXTENSIONS=core_functions;tpch",
                    f"-DEXTENSION_STATIC_BUILD={int(static_build)}",
                    "-DENABLE_SANITIZER=OFF",
                    "-DENABLE_UBSAN=OFF",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            reply = build / ".cmake/api/v1/reply"
            index = json.loads(next(reply.glob("index-*.json")).read_text())
            model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
            targets = {
                target["name"]: json.loads((reply / target["jsonFile"]).read_text())
                for target in model["configurations"][0]["targets"]
            }

            def libraries(name):
                return " ".join(
                    fragment["fragment"]
                    for fragment in targets[name]["link"]["commandFragments"]
                    if fragment["role"] == "libraries"
                )

            tpch_libraries = libraries("tpch_loadable_extension")
            self.assertIn("libtpch_extension.a", tpch_libraries)
            self.assertEqual("libduckdb_static.a" in tpch_libraries, static_build, tpch_libraries)
            # Static consumers still need the core archive after their extension archives.
            for name, extension in (("duckdb", "tpch"), ("static_link_smoke", "parquet")):
                linked = libraries(name)
                self.assertIn("libduckdb_static.a", linked)
                self.assertLess(linked.index(f"lib{extension}_extension.a"), linked.rindex("libduckdb_static.a"))

    @unittest.skipIf(sys.platform == "win32", "Windows requires EXTENSION_STATIC_BUILD")
    def test_dynamic_loadable_uses_host_duckdb(self):
        self.check_linkage(False)

    def test_static_loadable_includes_duckdb(self):
        self.check_linkage(True)


if __name__ == "__main__":
    unittest.main()
