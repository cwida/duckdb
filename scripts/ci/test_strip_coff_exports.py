import importlib.util
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "strip_coff_exports.py"
SPEC = importlib.util.spec_from_file_location("strip_coff_exports", SCRIPT)
EXPORTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORTS)
TOOLS = ["clang-cl", "llvm-lib", "lld-link", "llvm-readobj"]


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

    @unittest.skipUnless(all(shutil.which(tool) for tool in TOOLS), "Requires the LLVM Windows tools")
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


if __name__ == "__main__":
    unittest.main()
