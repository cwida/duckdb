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
    def check_linkage(self, static_build, wasm_source_build=False, prebuilt=False, sanitized=False):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            query = build / ".cmake/api/v1/query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            extra_flags = []
            if prebuilt:
                archives = build / "prebuilt"
                archives.mkdir()
                for name in ("duckdb_static", "duckdb_shell", "test_extension", "tpch_extension"):
                    filename = f"{name}.lib" if sys.platform == "win32" else f"lib{name}.a"
                    (archives / filename).touch()
                core_archive = "duckdb_static.lib" if sys.platform == "win32" else "libduckdb_static.a"
                extra_flags = [
                    f"-DPREBUILT_BINARY={archives / core_archive}",
                    "-DPREBUILT_EXTENSIONS=test;tpch",
                ]
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
                    "-DSTATICALLY_LINK_EXTENSIONS=core_functions;tpch;test",
                    f"-DEXTENSION_STATIC_BUILD={int(static_build)}",
                    f"-DWASM_LOADABLE_EXTENSIONS={int(wasm_source_build)}",
                    f"-DENABLE_SANITIZER={int(sanitized)}",
                    f"-DFORCE_ASSERT={int(sanitized)}",
                    "-DENABLE_UBSAN=OFF",
                    *extra_flags,
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

            if wasm_source_build:
                for name in ("test_extension", "test_loadable_extension"):
                    target = targets[name]
                    source = next(
                        source for source in target["sources"] if Path(source["path"]).name == "test_extension.cpp"
                    )
                    includes = target["compileGroups"][source["compileGroupIndex"]]["includes"]
                    for header in ("duckdb_test/test_bundle.hpp", "sqllogic_test_executor.hpp"):
                        self.assertTrue(
                            any((Path(include["path"]) / header).is_file() for include in includes),
                            f"{name} cannot resolve {header}",
                        )
                return

            def libraries(name):
                return " ".join(
                    fragment["fragment"]
                    for fragment in targets[name]["link"]["commandFragments"]
                    if fragment["role"] == "libraries"
                )

            tpch_libraries = libraries("tpch_loadable_extension")
            self.assertIn("tpch_extension", tpch_libraries)
            self.assertEqual("duckdb_static" in tpch_libraries, static_build, tpch_libraries)
            test_libraries = libraries("test_loadable_extension")
            self.assertEqual("duckdb_static" in test_libraries, static_build, test_libraries)
            for extension in ("core_functions_extension", "tpch_extension"):
                self.assertEqual(extension in test_libraries, static_build, test_libraries)
            test_archive = targets["test_extension"]["artifacts"][0]["path"].replace("\\", "/")
            self.assertEqual(test_libraries.replace("\\", "/").count(test_archive), 1, test_libraries)
            for name in ("test_loadable_extension", "duckdb_tests_loadable_extension"):
                linked = libraries(name)
                if sys.platform == "win32":
                    self.assertIn("duckdb_static", linked, f"{name}: {linked}")
                    for extension in ("core_functions_extension", "tpch_extension"):
                        self.assertIn(extension, linked, f"{name}: {linked}")
                    self.assertNotIn("tools/shell/duckdb.lib", linked.replace("\\", "/"), f"{name}: {linked}")
                elif name == "duckdb_tests_loadable_extension":
                    for library in ("duckdb_static", "core_functions_extension", "parquet_extension"):
                        self.assertNotIn(library, linked, f"{name}: {linked}")
            target_names = {target["id"]: name for name, target in targets.items()}

            def dependencies(name):
                return {target_names[dependency["id"]] for dependency in targets[name].get("dependencies", [])}

            self.assertIn("test_loadable_extension", dependencies("duckdb_local_extension_repo"))
            self.assertIn("tpch_loadable_extension", dependencies("duckdb_local_extension_repo"))
            if prebuilt:
                self.assertEqual(targets["duckdb_re2"]["type"], "STATIC_LIBRARY")
                self.assertIn("duckdb_re2", dependencies("test_loadable_extension"))
                self.assertEqual(
                    {Path(source["path"]).name for source in targets["test_extension"]["sources"]},
                    {"prebuilt_extension.cpp"},
                )
                self.assertNotIn("test_sqlite_runner", dependencies("test_extension"))
                self.assertNotIn("test_sqlite_runner", dependencies("test_loadable_extension"))
                test_prebuilt_archive = (
                    (archives / ("test_extension.lib" if sys.platform == "win32" else "libtest_extension.a"))
                    .relative_to(build)
                    .as_posix()
                )
                for name in ("shell", "test_loadable_extension"):
                    linked = libraries(name).replace("\\", "/")
                    self.assertLess(linked.index(test_prebuilt_archive), linked.index("test_helpers"), linked)
                    if (
                        name == "shell"
                        or (name == "test_loadable_extension" and static_build)
                        or sys.platform == "win32"
                    ):
                        self.assertLess(linked.index("test_helpers"), linked.rindex("duckdb_static"), linked)
            if sanitized:
                for name in ("test_extension", "shell"):
                    compile_flags = " ".join(
                        fragment["fragment"]
                        for group in targets[name]["compileGroups"]
                        for fragment in group["compileCommandFragments"]
                    )
                    self.assertIn("-fsanitize=address", compile_flags, name)
            # Static consumers still need the core archive after their extension archives.
            for name, extension in (("shell", "tpch"), ("static_link_smoke", "parquet")):
                linked = libraries(name)
                self.assertIn("duckdb_static", linked)
                self.assertLess(linked.index(f"{extension}_extension"), linked.rindex("duckdb_static"))

    @unittest.skipIf(sys.platform == "win32", "Windows requires EXTENSION_STATIC_BUILD")
    def test_dynamic_loadable_uses_host_duckdb(self):
        self.check_linkage(False)

    def test_static_loadable_includes_duckdb(self):
        self.check_linkage(True)

    def test_wasm_source_build_has_test_runner_headers(self):
        self.check_linkage(True, wasm_source_build=True)

    def test_prebuilt_extensions_are_packaged_without_recompiling_runner(self):
        self.check_linkage(True, prebuilt=True)

    @unittest.skipIf(sys.platform == "win32", "Windows requires EXTENSION_STATIC_BUILD")
    def test_prebuilt_dynamic_loadable_uses_host_duckdb(self):
        self.check_linkage(False, prebuilt=True)

    @unittest.skipIf(sys.platform == "win32", "Windows requires EXTENSION_STATIC_BUILD")
    def test_sanitized_loadable_uses_host_duckdb(self):
        self.check_linkage(False, sanitized=True)


if __name__ == "__main__":
    unittest.main()
