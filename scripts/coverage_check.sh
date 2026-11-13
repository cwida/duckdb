#!/bin/bash

set -e

# prepare coverage file
lcov --config-file .github/workflows/lcovrc --zerocounters --directory .
lcov --config-file .github/workflows/lcovrc --capture --initial --directory . --base-directory . --no-external --output-file coverage.info

# build with coverage enabled
mkdir -p build/coverage
(cd build/coverage && cmake -E env CXXFLAGS="--coverage" cmake -DDUCKDB_USE="core_functions;parquet;json;icu;httplib;loadable_extensions;local_extension_repository" -DDUCKDB_EXTEND=autocomplete -DENABLE_SANITIZER=0 -DCMAKE_BUILD_TYPE=Debug ../.. && cmake --build .)

# run tests
build/coverage/test/run
build/coverage/test/run "[detailed_profiler]"
build/coverage/test/run test/sql/tpch/tpch_sf01.test_slow
python3 -m pytest --shell-binary build/coverage/duckdb tools/shell/tests/

# finalize coverage file
lcov --config-file .github/workflows/lcovrc --directory . --base-directory . --no-external --capture --output-file coverage.info
lcov --config-file .github/workflows/lcovrc --remove coverage.info $(< .github/workflows/lcov_exclude) -o lcov.info

# generate coverage html
genhtml -o coverage_html lcov.info

# check that coverage passes threshold
# python3 scripts/check_coverage.py
