#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build/release}"
TEST_STATIC="${TEST_STATIC:-false}"
TEST_SHARED="${TEST_SHARED:-false}"

if [[ "$TEST_STATIC" == "true" ]]; then
	gcc -std=c11 \
		-Isrc/include \
		examples/embedded-c/main.c \
		"$BUILD_DIR/src/libduckdb_static.a" \
		-ldl -lpthread -lm -lstdc++ \
		-o "$BUILD_DIR/gcc-static-library-smoke"
	"$BUILD_DIR/gcc-static-library-smoke"
fi

"$BUILD_DIR/test/static_link_smoke"
"$BUILD_DIR/test/static_link_explicit_smoke"

if [[ "$TEST_SHARED" == "true" ]]; then
	gcc -std=c11 \
		-Isrc/include \
		examples/embedded-c/main.c \
		-L"$BUILD_DIR/src" \
		-Wl,-rpath,"$PWD/$BUILD_DIR/src" \
		-lduckdb \
		-o "$BUILD_DIR/gcc-shared-library-smoke"
	"$BUILD_DIR/gcc-shared-library-smoke"
fi
