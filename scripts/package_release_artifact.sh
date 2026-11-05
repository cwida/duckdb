#!/usr/bin/env bash

set -euo pipefail

usage() {
	echo "Usage: $0 <cli|cli-zip|shared-libs|static-libs> <artifact-suffix> <input> [...]" >&2
	exit 1
}

if [[ $# -lt 3 ]]; then
	usage
fi

artifact_kind="$1"
artifact_suffix="$2"
shift 2

if [[ ! "$artifact_suffix" =~ ^[a-z0-9][a-z0-9._-]*$ ]]; then
	echo "Invalid artifact suffix: $artifact_suffix" >&2
	exit 1
fi

case "$artifact_kind" in
cli)
	artifact_name="duckdb-cli-${artifact_suffix}.tar.gz"
	if [[ $# -ne 1 ]]; then
		usage
	fi
	;;
cli-zip)
	artifact_name="duckdb-cli-${artifact_suffix}.zip"
	if [[ $# -ne 1 ]]; then
		usage
	fi
	;;
shared-libs)
	artifact_name="duckdb-shared-libs-${artifact_suffix}.tar.gz"
	;;
static-libs)
	artifact_name="duckdb-static-libs-${artifact_suffix}.tar.gz"
	;;
*)
	usage
	;;
esac

output_dir="${ARTIFACT_OUTPUT_DIR:-.}"
if [[ ! -d "$output_dir" ]]; then
	echo "Artifact output directory does not exist: $output_dir" >&2
	exit 1
fi

staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/duckdb-release-artifact.XXXXXX")"
temporary_archive="$(mktemp "${output_dir}/.duckdb-release-artifact.XXXXXX")"
cleanup() {
	rm -rf "$staging_dir"
	rm -f "$temporary_archive"
}
trap cleanup EXIT

members=()
stage_file() {
	local source_path="$1"
	local member_name
	member_name="$(basename "$source_path")"

	if [[ ! -e "$source_path" && ! -L "$source_path" ]]; then
		echo "Release artifact input does not exist: $source_path" >&2
		exit 1
	fi
	if [[ -e "$staging_dir/$member_name" || -L "$staging_dir/$member_name" ]]; then
		echo "Duplicate release artifact member: $member_name" >&2
		exit 1
	fi

	cp -Pp "$source_path" "$staging_dir/$member_name"
	members+=("$member_name")
}

if [[ "$artifact_kind" == "cli" || "$artifact_kind" == "cli-zip" ]]; then
	cli_name="$(basename "$1")"
	if [[ "$cli_name" != "duckdb" && "$cli_name" != "duckdb.exe" ]]; then
		echo "CLI input must be named duckdb or duckdb.exe: $1" >&2
		exit 1
	fi
	stage_file "$1"
else
	for library in "$@"; do
		stage_file "$library"
	done

	script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	repository_root="$(cd "$script_dir/.." && pwd)"
	stage_file "$repository_root/src/include/duckdb.h"
	stage_file "$repository_root/src/include/duckdb_v2.h"
	stage_file "$repository_root/src/include/duckdb_extension.h"
	stage_file "$repository_root/src/include/duckdb_extension_v2.h"
	stage_file "$repository_root/src/include/duckdb_static_extension.h"
fi

if [[ "$artifact_kind" == "cli-zip" ]]; then
	"${PYTHON:-python3}" -m zipfile -c "$temporary_archive" "$staging_dir/${members[0]}"
else
	tar -C "$staging_dir" -czf "$temporary_archive" "${members[@]}"
fi
mv "$temporary_archive" "$output_dir/$artifact_name"

echo "Created $output_dir/$artifact_name"
if [[ "$artifact_kind" == "cli-zip" ]]; then
	"${PYTHON:-python3}" -m zipfile -l "$output_dir/$artifact_name"
else
	tar -tzf "$output_dir/$artifact_name"
fi
