# Note: tests for postgres_scanner are currently not run. All of them need a postgres server running. One test
#       uses a remote rds server but that's not something we want to run here.
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            GIT_URL https://github.com/duckdb/duckdb-postgres
            GIT_TAG 8b9d0bbe8de4c57fb90d9e2530f9ce39f8c4baf3
            APPLY_PATCHES
            SUBMODULES database-connector
            )
 endif()
