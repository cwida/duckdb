"""Test Windows static export removal and self-contained extension linkage."""

import importlib.util
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/strip_coff_exports.py"
SPEC = importlib.util.spec_from_file_location("strip_coff_exports", SCRIPT)
EXPORTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORTS)
LLVM_TOOLS = ["clang-cl", "llvm-lib", "lld-link", "llvm-readobj"]
CMAKE_TOOLS = ["cmake", "ninja", *LLVM_TOOLS]


def archive_member(data):
    header = (
        b"test.obj/".ljust(16)
        + b"0".ljust(12)
        + b"0".ljust(6)
        + b"0".ljust(6)
        + b"644".ljust(8)
        + str(len(data)).encode().ljust(10)
        + b"`\n"
    )
    return b"!<arch>\n" + header + data + (b"\n" if len(data) & 1 else b"")


class StripCoffExportsTests(unittest.TestCase):
    def test_bigobj_preserves_other_directives_and_archive_layout(self):
        directives = b' /DEFAULTLIB:libcmt /EXPORT:"?function@@YAHXZ" -export:data,DATA /INCLUDE:keep '
        header = (
            struct.pack("<HHHHI", 0, 0xFFFF, 2, 0xAA64, 0)
            + EXPORTS.BIGOBJ_CLASS_ID
            + struct.pack("<IIIIIII", 0, 0, 0, 0, 1, 0, 0)
        )
        section = b".drectve" + struct.pack("<IIIIIIHHI", 0, 0, len(directives), 96, 0, 0, 0, 0, 0x100A00)
        original = archive_member(header + section + directives)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bigobj.lib"
            path.write_bytes(original)
            self.assertEqual(EXPORTS.strip_exports(path), 2)
            changed = path.read_bytes()
            self.assertEqual(len(changed), len(original))
            self.assertEqual(changed[:164], original[:164])
            self.assertIn(b"/DEFAULTLIB:libcmt", changed)
            self.assertIn(b"/INCLUDE:keep", changed)
            self.assertNotIn(b"/EXPORT:", changed)
            self.assertNotIn(b"-export:", changed)
            self.assertEqual(EXPORTS.strip_exports(path), 0)

    def test_invalid_archive_is_not_modified(self):
        original = archive_member(b"") + b"truncated"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.lib"
            path.write_bytes(original)
            with self.assertRaises(ValueError):
                EXPORTS.strip_exports(path)
            self.assertEqual(path.read_bytes(), original)

    @unittest.skipUnless(all(shutil.which(tool) for tool in LLVM_TOOLS), "Requires the LLVM Windows tools")
    def test_real_archive_relinks_without_exports_and_preserves_shared_objects(self):
        source = """
__declspec(dllexport) volatile unsigned char unused_data[32768] = {1};
__declspec(dllexport) int unused_function(void) { return unused_data[0]; }
__declspec(dllexport) int extension_init(void) { return 42; }
"""
        for arch, triple in [
            ("x86", "i686-pc-windows-msvc"),
            ("x64", "x86_64-pc-windows-msvc"),
            ("arm64", "aarch64-pc-windows-msvc"),
        ]:
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "fixture.c").write_text(source)

                def run(*command):
                    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    return result.stdout

                run("clang-cl", f"--target={triple}", "/nologo", "/c", "/O2", "/Gy", "/Gw", "/GS-", "fixture.c")
                object_before = (root / "fixture.obj").read_bytes()
                run("llvm-lib", "/out:fixture.lib", "fixture.obj")
                archive_before = (root / "fixture.lib").read_bytes()
                common = [
                    "lld-link",
                    f"/machine:{arch}",
                    "/dll",
                    "/noentry",
                    "/nodefaultlib",
                    "/opt:ref,icf",
                    "/safeseh:no",
                ]
                run(*common, "/out:before.dll", "fixture.lib", "/export:extension_init")
                self.assertEqual(EXPORTS.strip_exports(root / "fixture.lib"), 3)
                self.assertEqual((root / "fixture.obj").read_bytes(), object_before)
                self.assertEqual((root / "fixture.lib").stat().st_size, len(archive_before))
                run(*common, "/out:after.dll", "fixture.lib", "/export:extension_init")
                run(*common, "/out:shared.dll", "fixture.obj")
                before = run("llvm-readobj", "--coff-exports", "before.dll")
                after = run("llvm-readobj", "--coff-exports", "after.dll")
                shared = run("llvm-readobj", "--coff-exports", "shared.dll")
                self.assertIn("Name: unused_function", before)
                self.assertIn("Name: unused_data", before)
                self.assertNotIn("Name: unused_function", after)
                self.assertNotIn("Name: unused_data", after)
                self.assertIn("Name: extension_init", after)
                self.assertIn("Name: unused_function", shared)
                self.assertIn("Name: unused_data", shared)
                self.assertLess((root / "after.dll").stat().st_size, (root / "before.dll").stat().st_size - 30000)


@unittest.skipUnless(
    all(shutil.which(tool) for tool in CMAKE_TOOLS), "Requires CMake, Ninja and the LLVM Windows tools"
)
class WindowsStaticExtensionLinkTests(unittest.TestCase):
    def test_archive_loadables_select_core_after_targets_exist(self):
        source = (ROOT / "src/CMakeLists.txt").read_text()
        first = source.index("if(", source.index("duckdb_make_native_lto_archive(duckdb_static)"))
        hook = source[first : source.index("\n#\n# FIXME", first)]
        tools = (ROOT / "extension/extension_build_tools.cmake").read_text()
        functions = "\n".join(
            re.search(r"^function\(" + name + r"\b.*?^endfunction\(\)", tools, re.M | re.S).group()
            for name in [
                "duckdb_link_static_extension_core",
                "build_loadable_extension_directory",
                "build_static_extension",
                "build_extension_library",
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            shutil.copyfile(ROOT / "scripts/strip_coff_exports.py", root / "scripts/strip_coff_exports.py")
            (root / "core.c").write_text(
                "__declspec(dllexport) volatile unsigned char unused_data[32768] = {1};\n"
                "__declspec(dllexport) int unused_function(void) { return unused_data[0]; }\n"
                "__declspec(dllexport) int used(void) { return 42; }\n"
                "__declspec(dllexport) volatile int used_data = 7;\n"
                "__declspec(dllexport) int force(void) { return 0; }\n"
            )
            (root / "extension.c").write_text(
                "int used(void);\n__declspec(dllexport) int fixture_duckdb_cpp_init(void) { return used(); }\n"
            )
            (root / "direct.c").write_text(
                '#include "duckdb/common/winapi.hpp"\n'
                "DUCKDB_API int used(void);\n"
                "DUCKDB_API extern volatile int used_data;\n"
                "int force(void);\n"
                "DUCKDB_EXTENSION_API int direct_duckdb_cpp_init(void) { return used() + used_data + force(); }\n"
            )
            (root / "toolchain.cmake").write_text(
                "set(CMAKE_SYSTEM_NAME Windows)\n"
                f'set(CMAKE_C_COMPILER "{Path(shutil.which("clang-cl")).as_posix()}")\n'
                f'set(CMAKE_CXX_COMPILER "{Path(shutil.which("clang-cl")).as_posix()}")\n'
                "set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)\n"
                "set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)\n"
                f'set(CMAKE_AR "{Path(shutil.which("llvm-lib")).as_posix()}")\n'
                f'set(CMAKE_LINKER "{Path(shutil.which("lld-link")).as_posix()}")\n'
                "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
            )
            (root / "CMakeLists.txt").write_text(
                """cmake_minimum_required(VERSION 3.20)
project(fixture C CXX)
find_package(Python3 REQUIRED)
option(WINDOWS_STATIC_LIBRARY_EXPORTS "Keep exports" OFF)
option(EXTENSION_STATIC_BUILD "Embed core" ON)
add_compile_options(/Gy /Gw /GS-)
add_compile_definitions(DUCKDB_BUILD_LIBRARY)
function(link_threads LIBRARY LINKAGE)
endfunction()
function(duckdb_make_native_lto_archive TARGET)
endfunction()
function(duckdb_add_extension_describe NAME KIND)
endfunction()
set(EXTENSION_CONFIG_BUILD OFF)
set(EXTENSION_TESTS_ONLY ON)
set(DUCKDB_NORMALIZED_VERSION fixture)
set(DUCKDB_MODULE_BASE_DIR "@REPO@")
set(DuckDB_BINARY_DIR "${CMAKE_BINARY_DIR}")
file(WRITE "${CMAKE_BINARY_DIR}/duckdb_platform_out" windows_amd64)
add_custom_target(duckdb_platform)
@FUNCTIONS@
# Match DuckDB's configure order: extensions precede src/CMakeLists.txt.
build_extension_library(fixture extension.c)
build_loadable_extension_directory(direct CPP extension/direct "" "" "" direct.c)
target_include_directories(direct_loadable_extension PRIVATE "@REPO@/src/include")
target_link_options(direct_loadable_extension PRIVATE "/MAP:${CMAKE_BINARY_DIR}/direct.map")
add_library(core_objects OBJECT core.c)
set(ALL_OBJECT_FILES $<TARGET_OBJECTS:core_objects>)
add_library(duckdb_static STATIC ${ALL_OBJECT_FILES})
@HOOK@
add_library(host SHARED extension.c)
target_link_libraries(host fixture_extension)
add_library(duckdb SHARED ${ALL_OBJECT_FILES})
foreach(target host fixture_loadable_extension direct_loadable_extension duckdb)
  target_link_options(${target} PRIVATE /noentry /nodefaultlib /safeseh:no /opt:ref,icf)
endforeach()
set(CORE_DEPENDENCY_TARGET fixture_extension)
if(TARGET fixture_prebuilt_extension)
  set(CORE_DEPENDENCY_TARGET fixture_prebuilt_extension)
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/extension-core.txt"
  CONTENT "$<TARGET_GENEX_EVAL:fixture_loadable_extension,$<TARGET_PROPERTY:${CORE_DEPENDENCY_TARGET},INTERFACE_LINK_LIBRARIES>>"
  TARGET fixture_loadable_extension)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/host-core.txt"
  CONTENT "$<TARGET_GENEX_EVAL:host,$<TARGET_PROPERTY:${CORE_DEPENDENCY_TARGET},INTERFACE_LINK_LIBRARIES>>"
  TARGET host)
""".replace(
                    "@REPO@", ROOT.as_posix()
                )
                .replace("@FUNCTIONS@", functions)
                .replace("@HOOK@", hook)
            )

            def run(*command):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout

            build = root / "build"
            original = None
            for keep, static, prebuilt in [
                ("ON", "ON", False),
                ("OFF", "ON", False),
                ("ON", "ON", False),
                ("OFF", "ON", False),
                ("ON", "ON", True),
                ("OFF", "ON", True),
                ("ON", "OFF", False),
                ("OFF", "OFF", False),
            ]:
                with self.subTest(exports=keep, static=static, prebuilt=prebuilt):
                    run(
                        "cmake",
                        "-S",
                        str(root),
                        "-B",
                        str(build),
                        "-G",
                        "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DCMAKE_C_STANDARD_LIBRARIES=",
                        "-DCMAKE_CXX_STANDARD_LIBRARIES=",
                        f"-DCMAKE_TOOLCHAIN_FILE={root / 'toolchain.cmake'}",
                        f"-DWINDOWS_STATIC_LIBRARY_EXPORTS={keep}",
                        f"-DEXTENSION_STATIC_BUILD={static}",
                        f"-DDUCKDB_EXTENSION_FIXTURE_PREBUILT_PATH={root / 'prebuilt.lib' if prebuilt else ''}",
                    )
                    run("cmake", "--build", str(build))
                    archive = build / "duckdb_static.lib"
                    obj = build / "CMakeFiles/core_objects.dir/core.c.obj"
                    current = (
                        archive.read_bytes(),
                        archive.stat().st_mtime_ns,
                        obj.read_bytes(),
                        obj.stat().st_mtime_ns,
                    )
                    if original is None:
                        original = current
                        shutil.copyfile(build / "fixture_extension.lib", root / "prebuilt.lib")
                    self.assertEqual(current, original)
                    host = run("llvm-readobj", "--coff-exports", str(build / "host.dll"))
                    dll = run("llvm-readobj", "--coff-exports", str(build / "duckdb.dll"))
                    extension_path = build / "extension/fixture/fixture.duckdb_extension"
                    extension = run("llvm-readobj", "--coff-exports", str(extension_path))
                    self.assertIn("Name: unused_function", host)
                    self.assertIn("Name: unused_data", host)
                    self.assertIn("Name: unused_function", dll)
                    self.assertIn("Name: unused_data", dll)
                    self.assertIn("Name: fixture_duckdb_cpp_init", extension)
                    exported = keep == "ON" and static == "ON"
                    self.assertEqual("Name: unused_function" in extension, exported)
                    self.assertEqual("Name: unused_data" in extension, exported)
                    dependency = (build / "extension-core.txt").read_text()
                    if static == "OFF":
                        self.assertEqual(dependency, "")
                    else:
                        self.assertEqual(dependency, "duckdb_static" if exported else "duckdb_static_no_exports")
                    self.assertEqual((build / "host-core.txt").read_text(), "duckdb_static")
                    if keep == "OFF" and static == "ON":
                        self.assertGreater((build / "host.dll").stat().st_size - extension_path.stat().st_size, 30000)
                    direct_path = build / "extension/direct/direct.duckdb_extension"
                    direct = run("llvm-readobj", "--coff-exports", str(direct_path))
                    self.assertIn("Name: direct_duckdb_cpp_init", direct)
                    self.assertEqual("Name: used\n" in direct, exported)
                    self.assertEqual("Name: used_data\n" in direct, exported)
                    self.assertEqual("Name: unused_function" in direct, exported)
                    self.assertEqual("Name: unused_data" in direct, exported)
                    direct_map = (build / "direct.map").read_text()
                    if static == "ON":
                        # References must retain core code/data even after their exports are removed.
                        self.assertRegex(direct_map, r"\sused\s")
                        self.assertRegex(direct_map, r"\sused_data\s")
                        self.assertNotIn("__imp_used", direct_map)
                    else:
                        imports = run("llvm-readobj", "--coff-imports", str(direct_path))
                        self.assertIn("Name: duckdb.dll", imports)
                        self.assertIn("Symbol: used ", imports)
                        self.assertIn("Symbol: used_data ", imports)


if __name__ == "__main__":
    unittest.main()
