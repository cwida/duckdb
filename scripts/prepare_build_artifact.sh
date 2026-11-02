#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "Usage: $0 <build_type>" >&2
	exit 1
fi

BUILD_TYPE="$1"
BUILD_DIR="${RELEASE_ARTIFACT_SOURCE_DIR:-build/$BUILD_TYPE}"
ARTIFACT_ROOT="${RELEASE_ARTIFACT_STAGING_DIR:-build/$BUILD_TYPE-artifact}"
ARTIFACT_DIR="$ARTIFACT_ROOT/$BUILD_TYPE"
ARTIFACT_TARBALL="${RELEASE_ARTIFACT_TARBALL:-build/$BUILD_TYPE-artifact.tar.gz}"

if [[ ! -d "$BUILD_DIR" ]]; then
	echo "Build directory '$BUILD_DIR' does not exist" >&2
	exit 1
fi

rm -rf "$ARTIFACT_ROOT"
rm -f "$ARTIFACT_TARBALL"
mkdir -p "$ARTIFACT_DIR/test/extension" "$ARTIFACT_DIR/src" "$(dirname "$ARTIFACT_TARBALL")"

copy_file() {
	local source_path="$1"
	local relative_path="$2"
	if [[ ! -e "$source_path" && ! -L "$source_path" ]]; then
		return
	fi
	if [[ -e "$ARTIFACT_DIR/$relative_path" || -L "$ARTIFACT_DIR/$relative_path" ]]; then
		return
	fi
	mkdir -p "$ARTIFACT_DIR/$(dirname "$relative_path")"
	cp -Pp "$source_path" "$ARTIFACT_DIR/$relative_path"
}

copy_by_basename() {
	local destination_dir="$1"
	shift
	local source_path
	for source_path in "$@"; do
		copy_file "$source_path" "$destination_dir/$(basename "$source_path")"
	done
}

shopt -s nullglob

cli_path=""
# Required by CI jobs that run the CLI from build/<type>/duckdb.
for candidate in "$BUILD_DIR/duckdb" "$BUILD_DIR/duckdb.exe"; do
	if [[ -f "$candidate" ]]; then
		cli_path="$(basename "$candidate")"
		copy_file "$candidate" "$cli_path"
		break
	fi
done

unittest_path=""
# Required by CI test jobs that run the prebuilt unittest binary and runner.
for candidate in "$BUILD_DIR/test/unittest" "$BUILD_DIR/test/unittest.exe"; do
	if [[ -f "$candidate" ]]; then
		unittest_path="test/$(basename "$candidate")"
		copy_file "$candidate" "$unittest_path"
		break
	fi
done
if [[ -z "$unittest_path" ]]; then
	echo "No unittest executable found in $BUILD_DIR/test" >&2
	exit 1
fi

runner_path=""
for runner_name in run run.py run.bat; do
	if [[ -f "$BUILD_DIR/test/$runner_name" ]]; then
		copy_file "$BUILD_DIR/test/$runner_name" "test/$runner_name"
		if [[ -z "$runner_path" || "$runner_name" == "run.py" ]]; then
			runner_path="test/$runner_name"
		fi
	fi
done
if [[ -z "$runner_path" ]]; then
	echo "No unittest runner wrapper found in $BUILD_DIR/test" >&2
	exit 1
fi

# Required by ADBC and other tests that need the shared library.
# Shared runtime libraries retain the conventional src/ location. DLLs emitted
# beside an executable are normalized into src/ as well.
copy_by_basename src "$BUILD_DIR"/src/*.so* "$BUILD_DIR"/src/*.dylib "$BUILD_DIR"/src/*.dll
copy_by_basename src "$BUILD_DIR"/*.dll "$BUILD_DIR"/test/*.dll

# Required by jobs that link against the prebuilt static library.
# Static and import libraries are optional. macOS slicing stages these in libs/.
copy_by_basename src \
	"$BUILD_DIR"/src/libduckdb*.a \
	"$BUILD_DIR"/src/duckdb*.lib \
	"$BUILD_DIR"/libs/*

for extension_library in "$BUILD_DIR"/extension/*/lib*_extension.a "$BUILD_DIR"/extension/*/*_extension.lib; do
	extension_name="$(basename "$(dirname "$extension_library")")"
	copy_file "$extension_library" "extension/$extension_name/$(basename "$extension_library")"
done
# Required by extension tests using build/<type>/test/extension/*.duckdb_extension.
for extension in "$BUILD_DIR"/test/extension/*.duckdb_extension; do
	copy_file "$extension" "test/extension/$(basename "$extension")"
done

# Required by tests that use the local extension repository under the build directory.
if [[ -d "$BUILD_DIR/repository" ]]; then
	cp -a "$BUILD_DIR/repository" "$ARTIFACT_DIR/"
else
	echo "No $BUILD_DIR/repository directory found"
fi

benchmark_path=""
# Required by regression jobs that run the prebuilt benchmark runner.
for candidate in "$BUILD_DIR/benchmark/benchmark_runner" "$BUILD_DIR/benchmark/benchmark_runner.exe"; do
	if [[ -f "$candidate" ]]; then
		benchmark_path="benchmark/$(basename "$candidate")"
		copy_file "$candidate" "$benchmark_path"
		mkdir -p "$ARTIFACT_DIR/test/sql/storage_version"
		cp -a test/sql/storage_version/. "$ARTIFACT_DIR/test/sql/storage_version/"
		break
	fi
done

# Executables travel in the tarball so their modes survive GitHub's artifact transport.
chmod 755 "$ARTIFACT_DIR/$unittest_path" "$ARTIFACT_DIR/$runner_path"
if [[ -n "$cli_path" ]]; then
	chmod 755 "$ARTIFACT_DIR/$cli_path"
fi
if [[ -n "$benchmark_path" ]]; then
	chmod 755 "$ARTIFACT_DIR/$benchmark_path"
fi

echo "$BUILD_TYPE artifact includes:"
find "$ARTIFACT_DIR" \( -type f -o -type l \) | sort | sed "s|^$ARTIFACT_DIR/|  |"
echo "$BUILD_TYPE artifact size:"
du -h -d 3 "$ARTIFACT_DIR" | sort -h

set -x

# Use a tarball so executable bits are preserved when passing build artifacts between jobs.
# Use -4 to balance compression ratio (small enough output size) with compression time (a few sec).
if command -v pigz >/dev/null 2>&1; then
	COMPRESSOR=(pigz -4)
else
	COMPRESSOR=(gzip -4)
fi

tar -C "$ARTIFACT_ROOT" -cf - "$BUILD_TYPE" | "${COMPRESSOR[@]}" > "$ARTIFACT_TARBALL"

ls -lh "$ARTIFACT_TARBALL"
