# Extensions built by the Linux release and regression jobs.
duckdb_extension_load(icu)
duckdb_extension_load(tpch)
duckdb_extension_load(tpcds)
include("${EXTENSION_CONFIG_BASE_DIR}/fts.cmake")
duckdb_extension_load(json)
include("${EXTENSION_CONFIG_BASE_DIR}/inet.cmake")
duckdb_extension_load(parquet)
duckdb_extension_load(autocomplete)
set(DUCKDB_USE ${DUCKDB_USE} autocomplete)
# test_utils is temporarily disabled.
# include("${EXTENSION_CONFIG_BASE_DIR}/test-utils.cmake")
include("${EXTENSION_CONFIG_BASE_DIR}/httpfs.cmake")
set(DUCKDB_EXTEND ${DUCKDB_EXTEND} tpch tpcds fts inet httpfs)
