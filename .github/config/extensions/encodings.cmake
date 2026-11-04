if (NOT MINGW AND NOT ${WASM_ENABLED} AND ${BUILD_COMPLETE_EXTENSION_SET})
duckdb_extension_load(encodings
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-encodings
        GIT_TAG 07ccb7250761f46f5a65be855240dda0700cbf36
        TEST_DIR test/sql
)
endif()
