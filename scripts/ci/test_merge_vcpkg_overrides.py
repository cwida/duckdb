#!/usr/bin/env python3

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts import sync_out_of_tree_extensions as sync

SCRIPT = REPO_ROOT / 'scripts/merge_vcpkg_deps.py'


class MergeVcpkgOverridesTest(unittest.TestCase):
    def merge(self, overrides, *, conflict=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'build/extension_configuration/vcpkg.json'
            output.parent.mkdir(parents=True)
            manifests = []
            for index, entries in enumerate(overrides):
                manifest = root / f'extension{index}.json'
                manifest.write_text(json.dumps({'dependencies': ['example'], 'overrides': entries}))
                manifests.append(str(manifest))
            env = os.environ.copy()
            env.pop('OPENSSL_VERSION_OVERRIDE', None)
            result = subprocess.run(
                [sys.executable, str(SCRIPT), *manifests], cwd=root, env=env, capture_output=True, text=True
            )
            if conflict:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"Conflicting vcpkg overrides for '{conflict}'", result.stderr)
                for manifest in manifests:
                    self.assertIn(manifest, result.stderr)
                self.assertFalse(output.exists())
                return
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(output.read_text())['overrides']

    def test_preserves_extension_pins_and_port_revisions(self):
        dlpack = {'name': 'dlpack', 'version': '0.8'}
        flatbuffers = {'name': 'flatbuffers', 'version': '24.3.25', 'port-version': 1}
        overrides = self.merge([[flatbuffers], [dlpack]])
        self.assertIn(dlpack, overrides)
        self.assertIn(flatbuffers, overrides)

    def test_deduplicates_identical_overrides(self):
        pin = {'name': 'dlpack', 'version': '0.8'}
        overrides = self.merge([[pin], [dict(pin, **{'port-version': 0, '$comment': 'same version'})]])
        self.assertEqual([override for override in overrides if override['name'] == 'dlpack'], [pin])

    def test_rejects_conflicting_versions_and_port_revisions(self):
        first = {'name': 'flatbuffers', 'version': '24.3.25'}
        for second in [dict(first, version='25.12.19'), dict(first, **{'port-version': 1})]:
            with self.subTest(second=second):
                self.merge([[first], [second]], conflict='flatbuffers')

    def test_preserves_other_version_schemes(self):
        pins = [
            {'name': 'a', 'version-semver': '1.2.3'},
            {'name': 'b', 'version-date': '2026-01-01'},
            {'name': 'c', 'version-string': 'release'},
        ]
        overrides = self.merge([pins])
        for pin in pins:
            self.assertIn(pin, overrides)

    def test_out_of_tree_merger_preserves_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            external_dir = root / 'external'
            extension_dir = external_dir / 'example'
            extension_dir.mkdir(parents=True)
            pin = {'name': 'flatbuffers', 'version': '24.3.25'}
            (extension_dir / 'vcpkg.json').write_text(json.dumps({'dependencies': ['example'], 'overrides': [pin]}))
            output_dir = root / 'build'

            with patch.dict(os.environ, {'VCPKG_TOOLCHAIN_PATH': '/unused/vcpkg.cmake'}, clear=True):
                sync.merge_vcpkg_manifests(['example'], external_dir, root, output_dir)

            overrides = json.loads((output_dir / 'vcpkg.json').read_text())['overrides']
            self.assertIn(pin, overrides)


if __name__ == '__main__':
    unittest.main()
