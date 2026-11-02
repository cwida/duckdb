#!/usr/bin/env python3

import contextlib
import io
import os
import shutil
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

CMAKE = shutil.which('cmake')
MAKE = shutil.which('make')


class ExtensionSyncTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {}, clear=True)
        self.env.start()
        self.addCleanup(self.env.stop)

    def parse(self, content):
        config = self.root / 'extension_config.cmake'
        config.write_text(content)
        return sync.parse_cmake_file(config)

    def test_quoted_submodule_and_git_arguments(self):
        extensions = self.parse(
            '''
            duckdb_extension_load(avro
                GIT_URL "https://example.com/avro"
                GIT_TAG "012345"
                SUBMODULES "third_party/avro-c"
                LOAD_TESTS APPLY_PATCHES
            )
            '''
        )
        self.assertEqual(
            extensions,
            [
                {
                    'name': 'avro',
                    'git_url': 'https://example.com/avro',
                    'git_tag': '012345',
                    'submodules': ['third_party/avro-c'],
                    'apply_patches': True,
                }
            ],
        )

    def test_comments_and_quoted_command_text_are_ignored(self):
        extensions = self.parse(
            '''
            # duckdb_extension_load(commented GIT_URL unused GIT_TAG unused)
            #[=[
            duckdb_extension_load(block_comment GIT_URL unused GIT_TAG unused)
            ]=]
            message("duckdb_extension_load(quoted GIT_URL unused GIT_TAG unused)")
            set(example [[duckdb_extension_load(bracket GIT_URL unused GIT_TAG unused)]])
            duckdb_extension_load(active
                GIT_URL "https://example.com/repo#fragment"
                GIT_TAG main # SUBMODULES ignored APPLY_PATCHES
            )
            '''
        )
        self.assertEqual([ext['name'] for ext in extensions], ['active'])
        self.assertEqual(extensions[0]['git_url'], 'https://example.com/repo#fragment')
        self.assertEqual(extensions[0]['submodules'], [])
        self.assertFalse(extensions[0]['apply_patches'])

    def test_submodule_lists_spaces_and_parentheses(self):
        extensions = self.parse(
            r'''
            duckdb_extension_load(avro GIT_URL url GIT_TAG revision
                SUBMODULES "third_party/avro-c;path with spaces" "path(with)parens" escaped\ path
                DONT_LINK LOAD_TESTS)
            '''
        )
        self.assertEqual(
            extensions[0]['submodules'],
            ['third_party/avro-c', 'path with spaces', 'path(with)parens', 'escaped path'],
        )

    def test_empty_and_bracket_submodules(self):
        extensions = self.parse(
            '''
            duckdb_extension_load(empty GIT_URL url GIT_TAG revision SUBMODULES "")
            duckdb_extension_load(bracket GIT_URL url GIT_TAG revision SUBMODULES [=[path with spaces]=])
            '''
        )
        self.assertEqual(extensions[0]['submodules'], [])
        self.assertEqual(extensions[1]['submodules'], ['path with spaces'])

    def test_includes_respect_comments_and_declaration_order(self):
        included_dir = self.root / 'extensions'
        included_dir.mkdir()
        (included_dir / 'active.cmake').write_text('duckdb_extension_load(active GIT_URL url GIT_TAG revision)')
        (included_dir / 'ignored.cmake').write_text('duckdb_extension_load(ignored GIT_URL url GIT_TAG revision)')
        extensions = self.parse(
            '''
            duckdb_extension_load(first GIT_URL url GIT_TAG revision)
            # include("${EXTENSION_CONFIG_BASE_DIR}/ignored.cmake")
            include("${EXTENSION_CONFIG_BASE_DIR}/active.cmake")
            '''
        )
        self.assertEqual([ext['name'] for ext in extensions], ['first', 'active'])

    def test_in_tree_extensions_are_ignored(self):
        self.assertEqual(
            self.parse('duckdb_extension_load(json)\nduckdb_extension_load(local SOURCE_DIR /tmp/local)'), []
        )

    def make_named_configs(self):
        config_dir = self.root / '.github' / 'config' / 'extensions'
        config_dir.mkdir(parents=True)
        for name in ['httpfs', 'avro', 'aws']:
            (config_dir / f'{name}.cmake').write_text(
                f'duckdb_extension_load({name} GIT_URL https://example.com/{name} GIT_TAG revision)'
            )

    def test_core_extensions_are_additive_and_deduplicated(self):
        self.make_named_configs()
        os.environ.update(BUILD_EXTENSIONS='avro;httpfs', CORE_EXTENSIONS="'httpfs;parquet;tpch'")
        self.assertEqual(list(sync.collect_extensions(self.root)), ['avro', 'httpfs'])

    def test_core_extensions_without_build_extensions(self):
        self.make_named_configs()
        os.environ['CORE_EXTENSIONS'] = "'httpfs;parquet;tpch'"
        self.assertEqual(list(sync.collect_extensions(self.root)), ['httpfs'])

    def test_make_combines_quoted_core_extensions_with_build_extensions(self):
        self.make_named_configs()
        self.assertEqual(
            list(sync.collect_extensions(self.root, "avro;'httpfs;parquet;tpch'")),
            ['avro', 'httpfs'],
        )

    def test_explicit_build_extensions_override_aliases(self):
        self.make_named_configs()
        os.environ.update(BUILD_EXTENSIONS='aws', DUCKDB_EXTENSIONS='aws', CORE_EXTENSIONS='httpfs')
        self.assertEqual(list(sync.collect_extensions(self.root, 'avro')), ['avro', 'httpfs'])

    def test_duckdb_extensions_alias(self):
        self.make_named_configs()
        os.environ.update(DUCKDB_EXTENSIONS='aws', CORE_EXTENSIONS='httpfs')
        self.assertEqual(list(sync.collect_extensions(self.root)), ['aws', 'httpfs'])

    def test_project_config_overrides_named_revision(self):
        self.make_named_configs()
        config = self.root / 'project.cmake'
        config.write_text(
            'duckdb_extension_load(httpfs GIT_URL https://example.com/project-httpfs GIT_TAG project-pin)'
        )
        extensions = sync.collect_extensions(self.root, 'httpfs;avro', str(config))
        self.assertEqual(list(extensions), ['httpfs', 'avro'])
        self.assertEqual(extensions['httpfs']['git_url'], 'https://example.com/project-httpfs')
        self.assertEqual(extensions['httpfs']['git_tag'], 'project-pin')

    def test_first_project_config_wins(self):
        self.make_named_configs()
        first, second = self.root / 'first.cmake', self.root / 'second.cmake'
        first.write_text('duckdb_extension_load(httpfs GIT_URL first-url GIT_TAG first-pin)')
        second.write_text('duckdb_extension_load(httpfs GIT_URL second-url GIT_TAG second-pin)')
        extensions = sync.collect_extensions(self.root, 'httpfs', f'{first};{second}')
        self.assertEqual(extensions['httpfs']['git_tag'], 'first-pin')

    def test_project_local_and_disabled_extensions_prevent_default_cloning(self):
        self.make_named_configs()
        config = self.root / 'project.cmake'
        config.write_text(
            '''
            duckdb_extension_load(httpfs SOURCE_DIR /local/httpfs)
            duckdb_extension_load(avro DONT_BUILD GIT_URL unused GIT_TAG unused)
            duckdb_extension_load(httpfs GIT_URL unused GIT_TAG unused)
            '''
        )
        self.assertEqual(sync.collect_extensions(self.root, 'httpfs;avro', str(config)), {})

    def test_custom_config_directory_from_environment_and_argument(self):
        self.make_named_configs()
        custom_dir = self.root / 'project-configs'
        custom_dir.mkdir()
        (custom_dir / 'httpfs.cmake').write_text(
            'duckdb_extension_load(httpfs GIT_URL project-url GIT_TAG project-pin)'
        )
        os.environ['EXTENSION_CONFIG_BASE_DIR'] = 'project-configs'
        extensions = sync.collect_extensions(self.root, 'httpfs')
        self.assertEqual(extensions['httpfs']['git_tag'], 'project-pin')
        extensions = sync.collect_extensions(self.root, 'httpfs', extension_config_base_dir='.github/config/extensions')
        self.assertEqual(extensions['httpfs']['git_tag'], 'revision')

    def test_custom_config_directory_does_not_fall_back_to_builtin_pins(self):
        self.make_named_configs()
        custom_dir = self.root / 'project-configs'
        custom_dir.mkdir()
        self.assertEqual(sync.collect_extensions(self.root, 'httpfs', extension_config_base_dir=custom_dir), {})

    def test_nested_project_includes_use_configured_base_directory(self):
        config_dir = self.root / 'project-configs'
        config_dir.mkdir()
        (config_dir / 'httpfs.cmake').write_text(
            'duckdb_extension_load(httpfs GIT_URL project-url GIT_TAG project-pin)'
        )
        project = self.root / 'project'
        project.mkdir()
        (project / 'extension_config.cmake').write_text('include("${CMAKE_CURRENT_LIST_DIR}/group.cmake")')
        (project / 'group.cmake').write_text('include("${EXTENSION_CONFIG_BASE_DIR}/httpfs.cmake")')
        extensions = sync.collect_extensions(
            self.root, extension_configs_arg='project/extension_config.cmake', extension_config_base_dir=config_dir
        )
        self.assertEqual(extensions['httpfs']['git_tag'], 'project-pin')

    def test_missing_explicit_config_and_directory_fail(self):
        with self.assertRaises(FileNotFoundError):
            sync.collect_extensions(self.root, extension_configs_arg='missing.cmake')
        with self.assertRaises(FileNotFoundError):
            sync.collect_extensions(self.root, extension_config_base_dir='missing')

    def test_recursive_project_include_fails(self):
        config = self.root / 'recursive.cmake'
        config.write_text('include("${CMAKE_CURRENT_LIST_DIR}/recursive.cmake")')
        with self.assertRaisesRegex(ValueError, 'Recursive extension config include'):
            sync.parse_cmake_file(config)

    @unittest.skipUnless(MAKE, 'Make is required for build command tests')
    def test_make_extension_selectors_enable_new_mode(self):
        os.environ.update(PATH=os.defpath, USE_MERGED_VCPKG_MANIFEST='1', VCPKG_TOOLCHAIN_PATH='/unused/vcpkg.cmake')

        selectors = [
            ['DUCKDB_EXTENSIONS=httpfs'],
            ['BUILD_EXTENSIONS=httpfs'],
            ['CORE_EXTENSIONS=httpfs'],
            ['BUILD_HTTPFS=1'],
            ['BUILD_ALL_EXT=1'],
            ['BUILD_ALL_IT_EXT=1'],
            ['BUILD_ALL_OOT_EXT=1'],
            ['EXTENSION_CONFIGS=/project/project.cmake'],
        ]
        for selector in selectors:
            with self.subTest(selector=selector):
                commands = subprocess.check_output(
                    [MAKE, '-Bn', 'reldebug', 'EXTENSION_CONFIG_BASE_DIR=/project/custom configs'] + selector,
                    cwd=REPO_ROOT,
                    text=True,
                    stderr=subprocess.STDOUT,
                )
                self.assertIn('sync_out_of_tree_extensions.py', commands)
                self.assertIn('--extension-config-base-dir "/project/custom configs"', commands)
                self.assertNotIn('-DEXTENSION_CONFIG_BUILD=TRUE', commands)
                self.assertNotIn('build/extension_configuration', commands)
                if selector[0].startswith('EXTENSION_CONFIGS='):
                    self.assertIn('--extension-configs "/project/project.cmake"', commands)
                    self.assertIn('-DDUCKDB_EXTENSION_CONFIGS="/project/project.cmake"', commands)

    @unittest.skipUnless(MAKE, 'Make is required for build command tests')
    def test_make_new_mode_override_and_explicit_activation(self):
        os.environ.update(PATH=os.defpath, USE_MERGED_VCPKG_MANIFEST='1', VCPKG_TOOLCHAIN_PATH='/unused/vcpkg.cmake')
        base_command = [
            MAKE,
            '-Bn',
            'reldebug',
            'BUILD_EXTENSIONS=httpfs',
            'EXTENSION_CONFIG_BASE_DIR=/project/custom configs',
            'EXTENSION_CONFIGS=/project/project.cmake',
        ]

        commands = subprocess.check_output(
            base_command + ['DUCKDB_NEW_EXTENSION_BUILD=0'],
            cwd=REPO_ROOT,
            text=True,
            stderr=subprocess.STDOUT,
        )
        self.assertIn('-DEXTENSION_CONFIG_BASE_DIR="/project/custom configs"', commands)
        self.assertIn('-DDUCKDB_EXTENSION_CONFIGS="/project/project.cmake"', commands)
        self.assertIn('-DEXTENSION_CONFIG_BUILD=TRUE', commands)
        self.assertNotIn('sync_out_of_tree_extensions.py', commands)

        commands = subprocess.check_output(
            [MAKE, '-Bn', 'reldebug', 'DUCKDB_NEW_EXTENSION_BUILD=1'],
            cwd=REPO_ROOT,
            text=True,
            stderr=subprocess.STDOUT,
        )
        self.assertIn('sync_out_of_tree_extensions.py', commands)
        self.assertNotIn('-DEXTENSION_CONFIG_BUILD=TRUE', commands)
        self.assertNotIn('build/extension_configuration', commands)

    @unittest.skipUnless(CMAKE, 'CMake is required for configuration integration tests')
    def test_cmake_and_sync_use_the_same_project_configuration(self):
        os.environ['PATH'] = os.defpath
        source = self.root / 'sample-source'
        source.mkdir()
        os.environ['DUCKDB_SAMPLE_DIRECTORY'] = str(source)
        config_dir = self.root / 'project-configs'
        config_dir.mkdir()
        named = config_dir / 'sample.cmake'
        project = self.root / 'project.cmake'
        for config, revision in [(named, 'named-pin'), (project, 'project-pin')]:
            config.write_text(
                f'duckdb_extension_load(sample GIT_URL https://example.invalid/sample '
                f'GIT_TAG {revision} EXTENSION_VERSION {revision})'
            )
        for use_project in (False, True):
            with self.subTest(use_project=use_project):
                build_dir = self.root / f'build-{use_project}'
                command = [
                    CMAKE,
                    '-S',
                    str(REPO_ROOT),
                    '-B',
                    str(build_dir),
                    '-DEXTENSION_CONFIG_BUILD=TRUE',
                    '-DBUILD_EXTENSIONS=sample',
                ]
                if use_project:
                    os.environ['EXTENSION_CONFIG_BASE_DIR'] = str(config_dir)
                    command.append(f'-DDUCKDB_EXTENSION_CONFIGS={project}')
                else:
                    command.append(f'-DEXTENSION_CONFIG_BASE_DIR={config_dir}')
                configured = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertEqual(configured.returncode, 0, configured.stdout)
                extensions = sync.collect_extensions(
                    REPO_ROOT, 'sample', str(project) if use_project else '', config_dir
                )
                self.assertIn(
                    f'sample, "{extensions["sample"]["git_tag"]}"',
                    (build_dir / 'extensions.csv').read_text(),
                )

    def test_sync_clones_only_required_submodule_and_reuses_checkout(self):
        os.environ.update(
            PATH=os.defpath,
            GIT_ALLOW_PROTOCOL='file',
            GIT_CONFIG_NOSYSTEM='1',
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_AUTHOR_NAME='Extension Sync Test',
            GIT_AUTHOR_EMAIL='test@example.com',
            GIT_COMMITTER_NAME='Extension Sync Test',
            GIT_COMMITTER_EMAIL='test@example.com',
        )

        def git(repo, *args):
            return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.PIPE, text=True).strip()

        def repo(name):
            directory = self.root / name
            directory.mkdir()
            git(directory, 'init')
            (directory / 'source.txt').write_text(name)
            git(directory, 'add', '.')
            git(directory, 'commit', '-m', 'Initial source')
            return directory

        docs = repo('docs')
        library = repo('library')
        git(library, 'submodule', 'add', str(docs), 'doc/theme')
        git(library, 'commit', '-am', 'Add documentation theme')
        extension = repo('extension')
        git(extension, 'submodule', 'add', str(library), 'third_party/library')
        git(extension, 'commit', '-am', 'Add library')
        revision = git(extension, 'rev-parse', 'HEAD')
        descriptor = self.parse(
            f'duckdb_extension_load(example GIT_URL "{extension}" GIT_TAG "{revision}" SUBMODULES "third_party/library")'
        )[0]
        external_dir = self.root / 'external'
        with contextlib.redirect_stdout(io.StringIO()):
            sync.sync_extension(descriptor, external_dir, self.root)
        checkout = external_dir / 'example'
        self.assertEqual((checkout / 'third_party/library/source.txt').read_text(), 'library')
        self.assertFalse((checkout / 'third_party/library/doc/theme/source.txt').exists())

        with patch.object(sync, 'run_cmd', wraps=sync.run_cmd) as commands, contextlib.redirect_stdout(io.StringIO()):
            sync.sync_extension(descriptor, external_dir, self.root)
        self.assertFalse(
            any(call.args[0][1] in ('clone', 'fetch', 'checkout', 'submodule') for call in commands.call_args_list)
        )
        self.assertEqual(git(checkout, 'rev-parse', 'HEAD'), revision)

    def test_sync_commits_and_exports_patches_inside_submodules(self):
        os.environ.update(
            PATH=os.defpath,
            GIT_ALLOW_PROTOCOL='file',
            GIT_CONFIG_NOSYSTEM='1',
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_AUTHOR_NAME='Extension Sync Test',
            GIT_AUTHOR_EMAIL='test@example.com',
            GIT_COMMITTER_NAME='Extension Sync Test',
            GIT_COMMITTER_EMAIL='test@example.com',
        )

        def git(repo, *args):
            return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.PIPE, text=True).strip()

        def repo(name):
            directory = self.root / name
            directory.mkdir()
            git(directory, 'init')
            git(directory, 'config', 'user.name', 'Extension Sync Test')
            git(directory, 'config', 'user.email', 'test@example.com')
            (directory / 'source.txt').write_text('original\n')
            git(directory, 'add', '.')
            git(directory, 'commit', '-m', 'Initial source')
            return directory

        library = repo('library')
        extension = repo('extension')
        git(extension, 'submodule', 'add', str(library), 'third_party/library')
        git(extension, 'commit', '-am', 'Add library')
        revision = git(extension, 'rev-parse', 'HEAD')

        patch_name = '0001-update-library.patch'
        patch_dir = self.root / '.github' / 'patches' / 'extensions' / 'example'
        patch_dir.mkdir(parents=True)
        patch_file = patch_dir / patch_name
        patch_file.write_text(
            '''diff --git a/third_party/library/source.txt b/third_party/library/source.txt
index 4b48dee..92efc5d 100644
--- a/third_party/library/source.txt
+++ b/third_party/library/source.txt
@@ -1 +1 @@
-original
+patched
'''
        )
        descriptor = self.parse(
            f'duckdb_extension_load(example GIT_URL "{extension}" GIT_TAG "{revision}" '
            'SUBMODULES "third_party/library" APPLY_PATCHES)'
        )[0]
        external_dir = self.root / 'external'

        with contextlib.redirect_stdout(io.StringIO()):
            sync.sync_extension(descriptor, external_dir, self.root)
        checkout = external_dir / 'example'
        self.assertEqual((checkout / 'third_party/library/source.txt').read_text(), 'patched\n')
        self.assertEqual(git(checkout, 'status', '--porcelain'), '')
        self.assertEqual(git(checkout, 'log', '-1', '--format=%s'), patch_name)
        self.assertEqual(git(checkout / 'third_party/library', 'log', '-1', '--format=%s'), patch_name)

        with patch.object(sync, 'run_cmd', wraps=sync.run_cmd) as commands, contextlib.redirect_stdout(io.StringIO()):
            sync.sync_extension(descriptor, external_dir, self.root)
        self.assertFalse(
            any(call.args[0][1] in ('clone', 'fetch', 'checkout', 'submodule') for call in commands.call_args_list)
        )

        os.environ['EXPORT_EXTENSION_PATCHES'] = '1'
        with contextlib.redirect_stdout(io.StringIO()):
            sync.sync_extension(descriptor, external_dir, self.root)
        exported = patch_file.read_text()
        self.assertIn('diff --git a/third_party/library/source.txt b/third_party/library/source.txt', exported)
        self.assertNotIn('Submodule third_party/library', exported)
        git(checkout, 'reset', '--hard', revision)
        git(checkout, 'submodule', 'update', '--force')
        git(checkout, 'apply', '--check', str(patch_file))


if __name__ == '__main__':
    unittest.main()
