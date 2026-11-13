################################################################################
# DuckDB extension base config
################################################################################
#
# This is the base DuckDB extension configuration file. The extensions loaded here are included in every DuckDB build.
# Note that this file is checked into version control; if you want to specify which extensions to load for local
# development, create `extension/extension_config_local.cmake` and specify extensions there.
# The local file is also loaded by the DuckDB CMake build but ignored by version control.

# These extensions and capabilities are built and linked by default.
set(DUCKDB_USE
    ${DUCKDB_USE}
    core_functions
    parquet
    json
    icu
    httplib
    loadable_extensions
    local_extension_repository)

duckdb_extension_load(core_functions)
duckdb_extension_load(parquet)
duckdb_extension_load(json)
duckdb_extension_load(icu)

