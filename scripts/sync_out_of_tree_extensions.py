#!/usr/bin/env python3
"""
Syncs out-of-tree extensions into extension/external/<name> directories.

Used when DUCKDB_NEW_EXTENSION_BUILD=1. Reads the following environment variables
to determine which extensions to sync:

  BUILD_EXTENSIONS   Semicolon-separated list of extension names
                     (e.g. "spatial;delta;postgres_scanner")
  DUCKDB_EXTENSIONS  Alias for BUILD_EXTENSIONS
  CORE_EXTENSIONS   Legacy extension list, added to BUILD_EXTENSIONS
  EXTENSION_CONFIGS  Semicolon-separated list of cmake config file paths
                     whose duckdb_extension_load(... GIT_URL ...) calls are parsed
  EXTENSION_CONFIG_BASE_DIR
                     Directory of named extension configs (defaults to
                     .github/config/extensions under the DuckDB source tree)

Explicit EXTENSION_CONFIGS take precedence over named extension configs.
Each config supplies GIT_URL, GIT_TAG, SUBMODULES, and APPLY_PATCHES. The extension
is then cloned (or updated) at extension/external/<name>.
"""

import argparse
import json
import os
import re
import sys
import subprocess
from pathlib import Path


def run_cmd(cmd, cwd=None, check=True):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        print(f"ERROR: command failed: {' '.join(str(c) for c in cmd)}", file=sys.stderr)
        if result.stdout:
            print(result.stdout, file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        sys.exit(1)
    return result


def cmake_tokens(content):
    """Tokenize literal arguments, parentheses and comments without evaluating CMake."""
    pattern = re.compile(
        r'(?P<bracket>#?\[(?P<equals>=*)\[.*?\](?P=equals)\])'
        r'|(?P<comment>#[^\n]*)'
        r'|(?P<quoted>"(?:\\.|[^"\\])*")'
        r'|(?P<paren>[()])'
        r'|(?P<argument>(?:\\.|[^\s()"#])+)',
        re.DOTALL,
    )
    for match in pattern.finditer(content):
        value = match.group()
        kind = match.lastgroup
        if kind == 'comment' or (kind == 'bracket' and value.startswith('#')):
            continue
        if kind == 'bracket':
            width = len(match.group('equals')) + 2
            value = value[width:-width]
            if value.startswith('\n'):
                value = value[1:]
        elif kind in ('quoted', 'argument'):
            if kind == 'quoted':
                value = value[1:-1]
            value = re.sub(r'\\\n|\\([^A-Za-z0-9;])', lambda m: m.group(1) or '', value)
        yield ('paren' if kind == 'paren' else 'argument'), value


def cmake_commands(content):
    tokens = iter(cmake_tokens(content))
    for kind, name in tokens:
        if kind != 'argument' or next(tokens, None) != ('paren', '('):
            continue
        depth = 1
        arguments = []
        for kind, value in tokens:
            if kind == 'paren':
                depth += 1 if value == '(' else -1
            if depth == 0:
                break
            arguments.append(value)
        yield name.lower(), arguments


def parse_cmake_file(cmake_path, extension_config_base_dir=None, include_non_remote=False, _include_stack=()):
    """
    Parse a cmake file and return a list of out-of-tree extension descriptors,
    i.e. duckdb_extension_load() calls that contain GIT_URL.

    Follows absolute includes and includes using EXTENSION_CONFIG_BASE_DIR or
    CMAKE_CURRENT_LIST_DIR. Non-remote declarations can be retained for precedence.

    Each descriptor is a dict with keys:
      name, git_url, git_tag, submodules (list, may be empty), apply_patches (bool)

    This reads literal declarations; it does not evaluate conditionals or variables.
    """
    cmake_path = Path(cmake_path).resolve()
    if cmake_path in _include_stack:
        raise ValueError(f"Recursive extension config include: {cmake_path}")
    include_stack = (*_include_stack, cmake_path)
    content = cmake_path.read_text(encoding='utf-8')

    extensions = []

    ext_config_base_dir = Path(extension_config_base_dir or cmake_path.parent / 'extensions').resolve()
    options = {'DONT_LINK', 'DONT_BUILD', 'LOAD_TESTS', 'APPLY_PATCHES'}
    keywords = options | {
        'SOURCE_DIR',
        'INCLUDE_DIR',
        'TEST_DIR',
        'GIT_URL',
        'GIT_TAG',
        'SUBMODULES',
        'EXTENSION_VERSION',
        'LINKED_LIBS',
    }
    for command, arguments in cmake_commands(content):
        if command == 'include' and arguments:
            include_path = arguments[0].replace('${EXTENSION_CONFIG_BASE_DIR}', str(ext_config_base_dir))
            include_path = include_path.replace('${CMAKE_CURRENT_LIST_DIR}', str(cmake_path.parent))
            included = Path(include_path)
            if included.is_absolute():
                if not included.exists() and 'OPTIONAL' in arguments[1:]:
                    continue
                extensions.extend(parse_cmake_file(included, ext_config_base_dir, include_non_remote, include_stack))
            continue
        if command != 'duckdb_extension_load' or not arguments:
            continue
        name = arguments[0]
        values = {}
        keyword = None
        for argument in arguments[1:]:
            if argument in keywords:
                keyword = argument
                values[keyword] = []
            elif keyword is not None:
                values[keyword].append(argument)
        remote = values.get('GIT_URL') and values.get('GIT_TAG') and 'DONT_BUILD' not in values
        if not remote and not include_non_remote:
            continue
        submodules = [path for argument in values.get('SUBMODULES', []) for path in argument.split(';') if path]

        extensions.append(
            {
                'name': name,
                'git_url': values['GIT_URL'][0] if remote else None,
                'git_tag': values['GIT_TAG'][0] if remote else None,
                'submodules': submodules,
                'apply_patches': 'APPLY_PATCHES' in values,
            }
        )

    return extensions


class ExtensionNotCleanError(Exception):
    """Raised when an extension repo has unexpected local state."""

    pass


def resolve_ref(repo_dir, ref):
    """
    Resolve a git ref to a full commit hash that exists locally (returns None on failure).

    --verify with the ^{commit} peel is required: a plain 'git rev-parse <sha>' echoes back
    any well-formed hash, even one whose object the local clone does not have.
    """
    result = run_cmd(['git', 'rev-parse', '--verify', '-q', ref + '^{commit}'], cwd=repo_dir, check=False)
    if result.returncode == 0:
        return result.stdout.strip()
    return None


def resolve_ref_or_fetch(name, repo_dir, ref):
    """Resolve <ref> locally, fetching from the remote if it is not known yet."""
    resolved = resolve_ref(repo_dir, ref)
    if resolved:
        return resolved
    print(f"  {name}: '{ref}' not found locally, fetching ...")
    run_cmd(['git', 'fetch', '--all', '--tags'], cwd=repo_dir)
    return resolve_ref(repo_dir, ref)


def get_patch_files(patch_dir):
    """Return sorted list of .patch filenames (not full paths) for an extension."""
    if patch_dir is None or not patch_dir.exists():
        return []
    return sorted(f for f in os.listdir(patch_dir) if f.endswith('.patch'))


def commit_patch(repo_dir, patch_name):
    run_cmd(['git', 'add', '-A'], cwd=repo_dir)
    run_cmd(
        [
            'git',
            '-c',
            'user.name=DuckDB Sync',
            '-c',
            'user.email=sync@duckdb.org',
            'commit',
            '-m',
            patch_name,
            '--no-verify',
        ],
        cwd=repo_dir,
    )


def apply_patches_as_commits(ext_dir, patch_dir, patches, submodules):
    """
    Apply each patch file and create commits whose message is the patch filename
    (e.g. "fix.patch"), including commits for changes inside submodules.
    """
    for patch_name in patches:
        patch_file = patch_dir / patch_name
        # Apply exactly as scripts/apply_extension_patches.py does for the FetchContent build, so a
        # patch that builds there builds here too: `patch -p1 --forward` tolerates the context drift
        # (fuzz) an extension bump can introduce, where `git apply` rejects a single changed context
        # line.  It writes to the working tree, not the index, so a patch may touch files inside a
        # checked-out submodule (e.g. database-connector/...); everything is staged explicitly below.
        # --no-backup-if-mismatch: a fuzzy apply otherwise leaves <file>.orig behind, which the
        # `git add -A` below would commit into the extension.
        run_cmd(['patch', '-p1', '--forward', '--no-backup-if-mismatch', '-i', str(patch_file)], cwd=ext_dir)
        # A superproject cannot stage file changes made inside a submodule. Commit those first so
        # the parent patch commit can record the resulting gitlink and remain clean on later syncs.
        for submodule in submodules:
            submodule_dir = ext_dir / submodule
            status = run_cmd(['git', 'status', '--porcelain'], cwd=submodule_dir)
            if status.stdout.strip():
                commit_patch(submodule_dir, patch_name)
        commit_patch(ext_dir, patch_name)


def _exportable_reason(commits_oldest_first):
    """
    Return None if the commit list can be exported as patch files, or a human-readable
    reason string explaining why it cannot.

    Commits are exportable when every message:
      - is a single word (no whitespace)
      - ends in '.patch'
      - is unique
      - appears in strict ascending lexicographic order
    """
    seen = set()
    for i, msg in enumerate(commits_oldest_first):
        if len(msg.split()) != 1:
            return f"commit message '{msg}' contains whitespace (must be a single word)"
        if not msg.endswith('.patch'):
            return f"commit message '{msg}' does not end in '.patch'"
        if msg in seen:
            return f"duplicate commit message '{msg}'"
        seen.add(msg)
        if i > 0 and msg <= commits_oldest_first[i - 1]:
            return f"commits not in ascending lexicographic order: " f"'{commits_oldest_first[i - 1]}' >= '{msg}'"
    return None


def export_commits_as_patches(name, ext_dir, resolved_git_tag, patch_dir):
    """
    Export every commit on top of <resolved_git_tag> as a patch file in <patch_dir>.

    Each commit message becomes the filename; its diff is written as the file content.
    Validates the same constraints as _exportable_reason before writing anything.
    """
    status = run_cmd(['git', 'status', '--porcelain'], cwd=ext_dir)
    if status.stdout.strip():
        raise ExtensionNotCleanError(
            f"Extension '{name}' has uncommitted changes; cannot export patches, please commit or discard and try again:\n"
            f"{status.stdout.rstrip()}"
        )

    log = run_cmd(['git', 'log', '--format=%H %s', f'{resolved_git_tag}..HEAD'], cwd=ext_dir)
    lines = [l.strip() for l in log.stdout.strip().splitlines() if l.strip()]
    if not lines:
        print(f"  {name}: no commits to export")
        return

    # Parse (hash, message) pairs, oldest first
    commits = []
    for line in reversed(lines):
        hash_, _, msg = line.partition(' ')
        commits.append((hash_, msg))

    messages = [msg for _, msg in commits]
    reason = _exportable_reason(messages)
    if reason:
        raise ExtensionNotCleanError(f"Extension '{name}': cannot export patches — {reason}")

    patch_dir.mkdir(parents=True, exist_ok=True)
    for commit_hash, msg in commits:
        # Capture raw bytes: text-mode capture would translate CRLF line endings
        # in the diff to LF, corrupting patches that touch CRLF files.
        # Expand gitlink changes back into ordinary file diffs so exported patches can be applied
        # to a fresh checkout with either `patch -p1` or `git apply`.
        diff_proc = subprocess.run(
            ['git', 'diff', '--submodule=diff', f'{commit_hash}^', commit_hash],
            cwd=ext_dir,
            capture_output=True,
            check=True,
        )
        diff = b''.join(
            line for line in diff_proc.stdout.splitlines(keepends=True) if not line.startswith(b'Submodule ')
        )
        (patch_dir / msg).write_bytes(diff)
        print(f"    Exported: {msg}")

    print(f"  {name}: wrote {len(commits)} patch(es) to .github/patches/extensions/{name}/")


def check_extension_clean(name, ext_dir, git_tag, patches):
    """
    Verify the extension repo is in a known-good state:

      1. No uncommitted working-tree changes (git status --porcelain is empty).
      2. The only commits on top of <git_tag> are exactly the patch commits, in
         application order (oldest first).

    Fetches from remote if <git_tag> cannot be resolved locally.

    Returns True  if the repo is already at the correct state (no update needed).
    Returns False if the base is not yet at <git_tag> (update needed but repo is clean).
    Raises ExtensionNotCleanError if either condition fails.
    """
    # Condition 1: working tree clean
    status = run_cmd(['git', 'status', '--porcelain'], cwd=ext_dir)
    if status.stdout.strip():
        raise ExtensionNotCleanError(
            f"Extension '{name}' has uncommitted changes:\n{status.stdout.rstrip()}\n"
            f"To resolve:\n"
            f"* FORCE_APPLY_PATCHES=1           — discard local changes and re-apply patches\n"
            f"* DUCKDB_SKIP_APPLYING_PATCHES=1  — skip patch checks and compile as-is"
        )

    # Resolve git_tag; fetch from remote if it is not known locally yet
    resolved = resolve_ref_or_fetch(name, ext_dir, git_tag)
    if not resolved:
        raise ExtensionNotCleanError(f"Extension '{name}': cannot resolve ref '{git_tag}'")

    # Condition 2: commits on top of git_tag are exactly the patch commits
    log = run_cmd(['git', 'log', '--format=%s', f'{resolved}..HEAD'], cwd=ext_dir)
    local_commits_newest_first = [l.strip() for l in log.stdout.strip().splitlines() if l.strip()]
    local_commits_oldest_first = list(reversed(local_commits_newest_first))

    if local_commits_oldest_first != patches:
        export_hint = ''
        if not _exportable_reason(local_commits_oldest_first):
            export_hint = '\n* EXPORT_EXTENSION_PATCHES=1     — export local commits as patch files'
        raise ExtensionNotCleanError(
            f"Extension '{name}' has unexpected local commits.\n"
            f"  Expected (patch commits): {patches}\n"
            f"  Found:                    {local_commits_oldest_first}\n"
            f"To resolve:\n"
            f"* Delete extension/external/{name} and re-run to re-sync\n"
            f"* FORCE_APPLY_PATCHES=1           — discard local changes and re-apply patches\n"
            f"* DUCKDB_SKIP_APPLYING_PATCHES=1  — skip patch checks and compile as-is" + export_hint
        )

    # Check whether the base commit is already the expected git_tag
    n = len(patches)
    base = run_cmd(['git', 'rev-parse', f'HEAD~{n}' if n else 'HEAD'], cwd=ext_dir).stdout.strip()
    return base == resolved


def sync_extension(ext, external_dir, repo_root):
    name = ext['name']
    git_url = ext['git_url']
    git_tag = ext['git_tag']
    submodules = ext['submodules']
    apply_patches_flag = ext['apply_patches']

    ext_dir = external_dir / name
    patch_dir = (repo_root / '.github' / 'patches' / 'extensions' / name) if apply_patches_flag else None
    patches = get_patch_files(patch_dir)
    skip_patches = os.environ.get('DUCKDB_SKIP_APPLYING_PATCHES') == '1'

    if not (ext_dir / '.git').exists():
        # ── Fresh clone ──────────────────────────────────────────────────────
        print(f"  Cloning {name} from {git_url} ...")
        external_dir.mkdir(parents=True, exist_ok=True)
        run_cmd(['git', 'clone', git_url, str(ext_dir)])
        run_cmd(['git', 'checkout', git_tag], cwd=ext_dir)

        if submodules:
            # --force so a submodule with a dirty working tree (e.g. from previously applied
            # patches) is hard-reset to its pinned commit before patches are re-applied.
            run_cmd(['git', 'submodule', 'update', '--init', '--force', '--'] + submodules, cwd=ext_dir)

        if patches and not skip_patches:
            apply_patches_as_commits(ext_dir, patch_dir, patches, submodules)

        print(f"  Cloned {name} @ {git_tag}")
    elif skip_patches:
        # ── Skip patches: leave the existing repo untouched ──────────────────
        print(f"  {name}: DUCKDB_SKIP_APPLYING_PATCHES set, skipping patch check and application")
    else:
        # ── Existing repo ────────────────────────────────────────────────────
        force = os.environ.get('FORCE_APPLY_PATCHES') == '1'
        export = os.environ.get('EXPORT_EXTENSION_PATCHES') == '1'

        if export:
            resolved = resolve_ref_or_fetch(name, ext_dir, git_tag)
            if not resolved:
                raise ExtensionNotCleanError(f"Extension '{name}': cannot resolve ref '{git_tag}'")
            export_commits_as_patches(
                name, ext_dir, resolved, patch_dir or (repo_root / '.github' / 'patches' / 'extensions' / name)
            )
            return
        elif force:
            print(f"  {name}: force-resetting to {git_tag[:12]} and re-applying patches ...")
            if not resolve_ref_or_fetch(name, ext_dir, git_tag):
                raise ExtensionNotCleanError(f"Extension '{name}': cannot resolve ref '{git_tag}'")
            run_cmd(['git', 'reset', '--hard', git_tag], cwd=ext_dir)
            run_cmd(['git', 'clean', '-fd'], cwd=ext_dir)
        else:
            # Raises ExtensionNotCleanError if the repo has unexpected local state.
            already_current = check_extension_clean(name, ext_dir, git_tag, patches)

            if already_current:
                print(f"  {name}: already at {git_tag[:12]}, skipping")
                return

            # Base is not yet at git_tag; strip the current patch commits, move
            # to the new base, and re-apply patches.
            print(f"  Updating {name} to {git_tag} ...")
            n = len(patches)
            if n:
                run_cmd(['git', 'reset', '--hard', f'HEAD~{n}'], cwd=ext_dir)
            run_cmd(['git', 'fetch', '--all'], cwd=ext_dir)
            run_cmd(['git', 'checkout', git_tag], cwd=ext_dir)

        if submodules:
            # --force so a submodule with a dirty working tree (e.g. from previously applied
            # patches) is hard-reset to its pinned commit before patches are re-applied.
            run_cmd(['git', 'submodule', 'update', '--init', '--force', '--'] + submodules, cwd=ext_dir)

        if patches:
            apply_patches_as_commits(ext_dir, patch_dir, patches, submodules)

        print(f"  {'Force-reset' if force else 'Updated'} {name} @ {git_tag}")


def collect_extensions(
    repo_root, build_extensions_arg=None, extension_configs_arg=None, extension_config_base_dir=None
):
    """Return a dict of name -> extension descriptor for all out-of-tree extensions to sync."""
    config_dir = extension_config_base_dir or os.environ.get('EXTENSION_CONFIG_BASE_DIR')
    extensions_config_dir = repo_root / (config_dir or '.github/config/extensions')
    if config_dir and not extensions_config_dir.is_dir():
        raise FileNotFoundError(f"Extension config directory does not exist: {extensions_config_dir}")

    raw_build_extensions = (
        build_extensions_arg or os.environ.get('BUILD_EXTENSIONS') or os.environ.get('DUCKDB_EXTENSIONS') or ''
    )
    raw_core_extensions = os.environ.get('CORE_EXTENSIONS', '')
    raw_extension_configs = extension_configs_arg or os.environ.get('EXTENSION_CONFIGS') or ''

    extensions = {}  # name -> descriptor (first seen wins)

    # Explicit project declarations take precedence, including local and disabled extensions.
    for config_path_str in re.split(r'[;]+', raw_extension_configs):
        config_path_str = config_path_str.strip()
        if not config_path_str:
            continue
        full_path = repo_root / config_path_str
        for ext in parse_cmake_file(full_path, extensions_config_dir, include_non_remote=True):
            extensions.setdefault(ext['name'], ext)

    # Legacy Makefiles may wrap the whole list in shell quotes.
    raw_extensions = ';'.join(value.strip("'\"") for value in (raw_build_extensions, raw_core_extensions))
    for name in re.split(r'[;,\s]+', raw_extensions):
        name = name.strip("'\"")
        if not name or name in extensions:
            continue
        cmake_path = extensions_config_dir / f'{name}.cmake'
        if not cmake_path.exists():
            continue  # in-tree extension — no cmake file needed
        for ext in parse_cmake_file(cmake_path, extensions_config_dir, include_non_remote=True):
            if ext['name'] == name and ext['name'] not in extensions:
                extensions[ext['name']] = ext

    return {name: ext for name, ext in extensions.items() if ext['git_url']}


VCPKG_BUILTIN_BASELINE = 'cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3'  # Release 2026.06.24
VCPKG_REGISTRY_BASELINE = 'd485389ad737bb05a5e8afd1fbde5672b559f19e'
VCPKG_REGISTRY_PACKAGES = ['avro-c', 'vcpkg-cmake']


def merge_vcpkg_manifests(synced_extension_names, external_dir, repo_root, output_dir, local_manifest_dirs=None):
    """
    Collect vcpkg.json files from all synced extensions, merge their dependencies
    and overlay configuration, and write the result to <output_dir>/vcpkg.json.

    local_manifest_dirs lists directories of "driving" (in-tree) extensions — the
    repos whose extension config was passed via --extension-configs. Their own
    vcpkg.json is folded into the merge as well, since sync only clones out-of-tree
    dependencies and would otherwise drop the driving extension's own dependencies.

    Overlay paths are stored as absolute paths so the manifest is valid regardless
    of which directory cmake is invoked from.
    """
    dependencies_str = []
    dependencies_dict = []
    overlay_ports = []
    overlay_triplets = []

    openssl_version = os.environ.get('OPENSSL_VERSION_OVERRIDE', '3.6.0')

    def fold_in_manifest(manifest_dir):
        # Read <manifest_dir>/vcpkg.json (if present) into the accumulators, resolving
        # its overlay paths relative to that directory. Returns True if it was read.
        vcpkg_json_path = manifest_dir / 'vcpkg.json'
        if not vcpkg_json_path.exists():
            return False

        base_dir = manifest_dir.resolve()

        with open(vcpkg_json_path, encoding='utf-8') as f:
            data = json.load(f)

        for dep in data.get('dependencies', []):
            if isinstance(dep, str):
                dependencies_str.append(dep)
            elif isinstance(dep, dict):
                dependencies_dict.append(dep)

        config = data.get('vcpkg-configuration', {})
        for rel_path in config.get('overlay-ports', []):
            overlay_ports.append(str((base_dir / rel_path).resolve()))
        for rel_path in config.get('overlay-triplets', []):
            overlay_triplets.append(str((base_dir / rel_path).resolve()))
        return True

    for name in synced_extension_names:
        fold_in_manifest(external_dir / name)

    for manifest_dir in local_manifest_dirs or []:
        fold_in_manifest(Path(manifest_dir))

    out_path = output_dir / 'vcpkg.json'
    out_path.parent.mkdir(parents=True, exist_ok=True)

    if not dependencies_str and not dependencies_dict:
        # an empty vcpkg.json counts as no dependency; the empty manifest keeps VCPKG_MANIFEST_DIR valid for cmake
        with open(out_path, 'w', encoding='utf-8') as f:
            json.dump({'dependencies': []}, f, ensure_ascii=False, indent=4)
            f.write('\n')
        print(f"  Wrote {out_path} with no dependencies.")
        return

    if not os.environ.get('VCPKG_TOOLCHAIN_PATH'):
        sources_with_vcpkg = [n for n in synced_extension_names if (external_dir / n / 'vcpkg.json').exists()]
        sources_with_vcpkg += [str(d) for d in (local_manifest_dirs or []) if (Path(d) / 'vcpkg.json').exists()]
        print(
            f"ERROR: the following extensions have vcpkg dependencies but VCPKG_TOOLCHAIN_PATH is not set: "
            f"{', '.join(sources_with_vcpkg)}",
            file=sys.stderr,
        )
        sys.exit(1)

    # Deduplicate (preserve order, string deps after dict deps)
    seen = set()
    final_deps = []
    for dep in dependencies_dict:
        key = dep['name']
        if key not in seen:
            final_deps.append(dep)
            seen.add(key)
    for dep in dependencies_str:
        if dep not in seen:
            final_deps.append(dep)
            seen.add(dep)

    # Deduplicate overlay paths (local and out-of-tree extensions may share one)
    overlay_ports = list(dict.fromkeys(overlay_ports))
    overlay_triplets = list(dict.fromkeys(overlay_triplets))

    manifest = {
        'description': "Auto-generated vcpkg.json for combined DuckDB extension build, generated by 'scripts/sync_out_of_tree_extensions.py'",
        'builtin-baseline': VCPKG_BUILTIN_BASELINE,
        'dependencies': final_deps,
        'overrides': [{'name': 'openssl', 'version': openssl_version}],
        'vcpkg-configuration': {
            'registries': [
                {
                    'kind': 'git',
                    'repository': 'https://github.com/duckdb/vcpkg-duckdb-ports',
                    'baseline': VCPKG_REGISTRY_BASELINE,
                    'packages': VCPKG_REGISTRY_PACKAGES,
                }
            ]
        },
    }

    if overlay_ports:
        manifest['vcpkg-configuration']['overlay-ports'] = overlay_ports
    if overlay_triplets:
        manifest['vcpkg-configuration']['overlay-triplets'] = overlay_triplets

    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=4)
        f.write('\n')

    dep_names = [d if isinstance(d, str) else d['name'] for d in final_deps]
    print(f"  Wrote {out_path} with dependencies: {dep_names}")


def main():
    repo_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build-extensions', default=None, help='Semicolon-separated list of extension names')
    parser.add_argument('--extension-configs', default=None, help='Semicolon-separated list of cmake config file paths')
    parser.add_argument(
        '--extension-config-base-dir',
        default=None,
        help='Directory of named extension configs (relative paths are resolved from the DuckDB source tree)',
    )
    parser.add_argument(
        '--output-dir',
        default=str(repo_root / 'build'),
        help='Directory to write the merged vcpkg.json into (default: build/)',
    )
    args = parser.parse_args()

    external_dir = repo_root / 'extension' / 'external'
    output_dir = Path(args.output_dir)

    extensions = collect_extensions(
        repo_root, args.build_extensions, args.extension_configs, args.extension_config_base_dir
    )

    # The directory of each --extension-configs file is the driving extension's repo
    # root; fold its own vcpkg.json (if any) into the merged manifest so the driving
    # extension's dependencies are installed alongside the out-of-tree ones.
    local_manifest_dirs = []
    raw_extension_configs = args.extension_configs or os.environ.get('EXTENSION_CONFIGS') or ''
    for config_path_str in re.split(r'[;]+', raw_extension_configs):
        config_path_str = config_path_str.strip()
        if not config_path_str:
            continue
        full_path = Path(config_path_str) if os.path.isabs(config_path_str) else repo_root / config_path_str
        manifest_dir = full_path.parent.resolve()
        if (manifest_dir / 'vcpkg.json').exists() and manifest_dir not in local_manifest_dirs:
            local_manifest_dirs.append(manifest_dir)

    if not extensions:
        print("No out-of-tree extensions to sync.")
    else:
        print(f"Syncing out-of-tree extensions into extension/external/: {', '.join(extensions)}")
        for ext in extensions.values():
            try:
                sync_extension(ext, external_dir, repo_root)
            except ExtensionNotCleanError as e:
                print(f"\nERROR: {e}", file=sys.stderr)
                sys.exit(1)

    # Always write vcpkg.json into the requested output directory so that
    # VCPKG_MANIFEST_DIR can be set unconditionally in the Makefile.  When no
    # extensions need vcpkg the file contains an empty dependency list and
    # CMake/vcpkg will install nothing.
    merge_vcpkg_manifests(list(extensions.keys()), external_dir, repo_root, output_dir, local_manifest_dirs)

    print("Sync complete.")


if __name__ == '__main__':
    main()
