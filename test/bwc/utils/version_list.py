import re
import subprocess
from collections import OrderedDict


SEMVER_TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def parse_version_tag(tag):
    match = SEMVER_TAG_RE.match(tag)
    if not match:
        return None
    return tuple(int(part) for part in match.groups())


def list_supported_duckdb_versions(min_version="v1.1.0"):
    min_tuple = parse_version_tag(min_version)
    if min_tuple is None:
        raise ValueError(f"Invalid minimum version tag: {min_version}")

    result = subprocess.run(
        ["git", "tag", "--list", "v*"],
        capture_output=True,
        text=True,
        check=True,
    )
    raw_tags = [line.strip() for line in result.stdout.splitlines() if line.strip()]

    versions = []
    for tag in raw_tags:
        parsed = parse_version_tag(tag)
        if parsed is None:
            continue
        if parsed >= min_tuple:
            versions.append((parsed, tag))

    versions.sort(key=lambda entry: entry[0])
    return [tag for _, tag in versions]


def list_supported_duckdb_version_groups(min_version="v1.1.0"):
    grouped = OrderedDict()
    for version in list_supported_duckdb_versions(min_version=min_version):
        parsed = parse_version_tag(version)
        assert parsed is not None
        major, minor, _ = parsed
        group = f"v{major}.{minor}"
        if group not in grouped:
            grouped[group] = []
        grouped[group].append(version)
    return [{"group": group, "versions": versions} for group, versions in grouped.items()]
