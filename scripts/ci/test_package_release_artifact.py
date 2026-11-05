#!/usr/bin/env python3

import os
import stat
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class PackageReleaseArtifactTest(unittest.TestCase):
    def run_make(self, target: str, output_dir: Path, **variables: str) -> None:
        environment = os.environ.copy()
        environment["ARTIFACT_OUTPUT_DIR"] = str(output_dir)
        command = ["make", target]
        command.extend(f"{name}={value}" for name, value in variables.items())
        subprocess.run(command, cwd=REPO_ROOT, env=environment, check=True, capture_output=True, text=True)

    def test_cli_release_artifact(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            cli = root / "duckdb"
            cli.write_bytes(b"binary")
            cli.chmod(0o755)

            self.run_make(
                "cli-release-artifact",
                root,
                ARTIFACT_SUFFIX="linux-amd64",
                CLI_BINARY=str(cli),
            )

            archive_path = root / "duckdb-cli-linux-amd64.tar.gz"
            with tarfile.open(archive_path, mode="r:gz") as archive:
                members = archive.getmembers()
                self.assertEqual([member.name for member in members], ["duckdb"])
                self.assertTrue(members[0].mode & stat.S_IXUSR)
                self.assertEqual(archive.extractfile(members[0]).read(), b"binary")

    def test_cli_zip_release_artifact(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            cli = root / "duckdb.exe"
            cli.write_bytes(b"binary")

            self.run_make(
                "cli-zip-release-artifact",
                root,
                ARTIFACT_SUFFIX="windows-amd64",
                CLI_BINARY=str(cli),
            )

            archive_path = root / "duckdb-cli-windows-amd64.zip"
            with zipfile.ZipFile(archive_path) as archive:
                self.assertEqual(archive.namelist(), ["duckdb.exe"])
                self.assertEqual(archive.getinfo("duckdb.exe").compress_type, zipfile.ZIP_DEFLATED)
                self.assertEqual(archive.read("duckdb.exe"), b"binary")

    def test_shared_libraries_release_artifact(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            versioned_library = root / "libduckdb.so.1"
            versioned_library.write_bytes(b"library")
            library_link = root / "libduckdb.so"
            library_link.symlink_to(versioned_library.name)

            self.run_make(
                "shared-libs-release-artifact",
                root,
                ARTIFACT_SUFFIX="linux-amd64",
                SHARED_LIBRARIES=f"{library_link} {versioned_library}",
            )

            archive_path = root / "duckdb-shared-libs-linux-amd64.tar.gz"
            with tarfile.open(archive_path, mode="r:gz") as archive:
                members = {member.name: member for member in archive.getmembers()}
                self.assertEqual(
                    set(members),
                    {
                        "libduckdb.so",
                        "libduckdb.so.1",
                        "duckdb.h",
                        "duckdb_v2.h",
                        "duckdb_extension.h",
                        "duckdb_extension_v2.h",
                        "duckdb_static_extension.h",
                    },
                )
                self.assertTrue(members["libduckdb.so"].issym())
                self.assertEqual(members["libduckdb.so"].linkname, "libduckdb.so.1")

    def test_static_libraries_release_artifact(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            libraries = root / "libs"
            libraries.mkdir()
            library = libraries / "libduckdb_static.a"
            library.write_bytes(b"library")
            shell = libraries / "libduckdb_shell.a"
            shell.write_bytes(b"shell")
            extension = libraries / "libcore_functions_extension.a"
            extension.write_bytes(b"extension")

            self.run_make(
                "static-libs-release-artifact",
                root,
                ARTIFACT_SUFFIX="linux-amd64",
                STATIC_LIBRARIES=str(libraries / "*"),
            )

            archive_path = root / "duckdb-static-libs-linux-amd64.tar.gz"
            with tarfile.open(archive_path, mode="r:gz") as archive:
                members = {member.name: member for member in archive.getmembers()}
                self.assertEqual(
                    set(members),
                    {
                        "libduckdb_static.a",
                        "libduckdb_shell.a",
                        "libcore_functions_extension.a",
                        "duckdb.h",
                        "duckdb_v2.h",
                        "duckdb_extension.h",
                        "duckdb_extension_v2.h",
                        "duckdb_static_extension.h",
                    },
                )
                self.assertEqual(archive.extractfile(members["libduckdb_static.a"]).read(), b"library")
                self.assertEqual(archive.extractfile(members["libduckdb_shell.a"]).read(), b"shell")
                self.assertEqual(archive.extractfile(members["libcore_functions_extension.a"]).read(), b"extension")

    def test_release_artifact_normalizes_windows_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "windows-build"
            (source / "src").mkdir(parents=True)
            (source / "test" / "extension").mkdir(parents=True)
            (source / "extension" / "json").mkdir(parents=True)

            for path in [source / "duckdb.exe", source / "test" / "unittest.exe"]:
                path.write_bytes(b"executable")
                path.chmod(0o644)
            (source / "test" / "run.py").write_text("# runner\n", encoding="utf-8")
            (source / "test" / "run.bat").write_text("@echo off\n", encoding="utf-8")
            (source / "src" / "duckdb.dll").write_bytes(b"shared")
            (source / "src" / "duckdb_static.lib").write_bytes(b"static")
            (source / "extension" / "json" / "json_extension.lib").write_bytes(b"extension library")
            (source / "test" / "extension" / "demo.duckdb_extension").write_bytes(b"test extension")

            archive_path = root / "release-artifact.tar.gz"
            environment = os.environ.copy()
            subprocess.run(
                [
                    "make",
                    "release-artifact",
                    f"RELEASE_ARTIFACT_SOURCE_DIR={source}",
                    f"RELEASE_ARTIFACT_STAGING_DIR={root / 'staging'}",
                    f"RELEASE_ARTIFACT_TARBALL={archive_path}",
                ],
                cwd=REPO_ROOT,
                env=environment,
                check=True,
                capture_output=True,
                text=True,
            )

            with tarfile.open(archive_path, mode="r:gz") as archive:
                members = {member.name: member for member in archive.getmembers()}
                expected = {
                    "release/duckdb.exe",
                    "release/test/unittest.exe",
                    "release/test/run.py",
                    "release/test/run.bat",
                    "release/src/duckdb.dll",
                    "release/src/duckdb_static.lib",
                    "release/extension/json/json_extension.lib",
                    "release/test/extension/demo.duckdb_extension",
                }
                self.assertTrue(expected.issubset(members))
                self.assertTrue(members["release/duckdb.exe"].mode & stat.S_IXUSR)
                self.assertTrue(members["release/test/unittest.exe"].mode & stat.S_IXUSR)


if __name__ == "__main__":
    unittest.main()
