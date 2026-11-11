import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import version


class TestVersion(unittest.TestCase):
    def test_development_version(self):
        with (
            patch("version.release_version", return_value="2.0"),
            patch("version.next_patch_version", return_value=1),
            patch("version.commit_count", return_value=42),
        ):
            self.assertEqual(version.resolve_version(), "v2.0.1-dev42")

    def test_alpha_version(self):
        with patch("version.release_version", return_value="2.0"), patch("version.next_patch_version", return_value=1):
            self.assertEqual(version.resolve_version(alpha_run_number="123"), "v2.0.1-alpha123")

    def test_next_patch_version_without_release_tags(self):
        with patch("subprocess.check_output", return_value=""):
            self.assertEqual(version.next_patch_version("2.0"), 0)

    def test_next_patch_version_uses_highest_stable_tag(self):
        tags = "\n".join(
            [
                "v2.0.1",
                "v2.0.3-rc1",
                "v2.1.4",
                "v2.0.invalid",
                "v2.0.3",
                "v2.0.2-alpha1",
            ]
        )
        with patch("subprocess.check_output", return_value=tags) as check_output:
            self.assertEqual(version.next_patch_version("2.0"), 4)
            check_output.assert_called_once_with(["git", "tag", "--merged", "HEAD", "--list", "v2.0.*"], text=True)

    def test_next_patch_version_propagates_git_errors(self):
        for error in [OSError("git unavailable"), subprocess.CalledProcessError(1, "git")]:
            with self.subTest(error=error):
                with patch("subprocess.check_output", side_effect=error):
                    with self.assertRaises(type(error)):
                        version.next_patch_version("2.0")

    def test_explicit_versions(self):
        for explicit_version in ["v2.0.0", "v2.0.0-dev42", "v2.0.0-alpha42", "v2.0.0-rc1"]:
            with patch("version.next_patch_version") as next_patch_version:
                self.assertEqual(version.resolve_version(explicit_version), explicit_version)
                next_patch_version.assert_not_called()

    def test_invalid_explicit_version(self):
        with self.assertRaisesRegex(ValueError, "Invalid DuckDB version"):
            version.resolve_version("2.0.0")

    def test_release_build(self):
        self.assertTrue(version.is_release_build("v2.0.0"))
        self.assertTrue(version.is_release_build("v2.0.0-alpha42"))
        self.assertFalse(version.is_release_build("v2.0.0-rc1"))
        self.assertFalse(version.is_release_build("v2.0.0-dev42"))

    def test_invalid_release_version(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            version_file = Path(temp_dir) / "release_version.txt"
            version_file.write_text("v2.0\n", encoding="utf8")
            with patch.object(version, "RELEASE_VERSION_FILE", version_file):
                with self.assertRaisesRegex(ValueError, "Invalid release version"):
                    version.release_version()

    def test_legacy_environment_version(self):
        with patch.dict(os.environ, {"OVERRIDE_GIT_DESCRIBE": "v2.0.0-rc1"}, clear=True):
            self.assertEqual(version.environment_version(), "v2.0.0-rc1")

    def test_duckdb_environment_version_takes_precedence(self):
        environment = {
            "DUCKDB_VERSION": "v2.1.0-dev42",
            "OVERRIDE_GIT_DESCRIBE": "v2.0.0-rc1",
        }
        with patch.dict(os.environ, environment, clear=True):
            self.assertEqual(version.environment_version(), "v2.1.0-dev42")

    def test_explicit_version_takes_precedence_over_environment(self):
        with patch.dict(os.environ, {"DUCKDB_VERSION": "v2.1.0-dev42"}, clear=True):
            self.assertEqual(version.environment_version("v2.2.0-dev1"), "v2.2.0-dev1")

    def test_explicit_commit(self):
        self.assertEqual(version.resolve_commit("ABCDEF0123456789"), "abcdef0123456789")

    def test_commit_uses_full_hash(self):
        with patch("subprocess.check_output", return_value="abcdef0123456789\n") as check_output:
            self.assertEqual(version.resolve_commit(), "abcdef0123456789")
            check_output.assert_called_once_with(["git", "log", "-1", "--format=%H"], text=True)

    def test_missing_git_fallbacks(self):
        with patch("subprocess.check_output", side_effect=subprocess.CalledProcessError(1, "git")):
            self.assertEqual(version.resolve_commit(), "0123456789")
            self.assertEqual(version.commit_count(), 0)


if __name__ == "__main__":
    unittest.main()
