#!/usr/bin/env bash
# Regenerates the V1 C API headers from api_spec/v1/, then formats the outputs.
# Invoked manually after editing YAML, or via make generate-files.
# Set CAPIGEN_PYTHON to a Python that has capigen installed (make generate-files
# does this via the capigen_venv target), or put `capigen` on PATH yourself.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

CAPIGEN_PYTHON="${CAPIGEN_PYTHON:-}"
if [ -n "$CAPIGEN_PYTHON" ]; then
	capigen() { "$CAPIGEN_PYTHON" -m capigen "$@"; }
	FORMAT_PYTHON="${FORMAT_PYTHON:-$CAPIGEN_PYTHON}"
elif ! command -v capigen >/dev/null 2>&1; then
	echo "error: capigen not found - run 'make capigen_venv' and re-run with CAPIGEN_PYTHON=.cache/capigen-venv/bin/python" >&2
	exit 1
fi
FORMAT_PYTHON="${FORMAT_PYTHON:-python3}"

capigen c \
	--spec-dir api_spec/v1 \
	-o src/include/duckdb.h

capigen extension_header \
	--spec-dir api_spec/v1 \
	--template api_spec/v1/extension/duckdb_extension.h.in \
	--internal-out src/include/duckdb/main/capi/extension_api.hpp \
	-o src/include/duckdb_extension.h

printf '%s\n' \
	src/include/duckdb.h \
	src/include/duckdb_extension.h \
	src/include/duckdb/main/capi/extension_api.hpp |
	"$FORMAT_PYTHON" scripts/format.py --files-from - --fix --noconfirm --skip-typos
