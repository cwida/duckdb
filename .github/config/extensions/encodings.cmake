if (NOT MINGW AND NOT ${WASM_ENABLED} AND ${BUILD_COMPLETE_EXTENSION_SET})
duckdb_extension_load(encodings
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-encodings
        GIT_TAG 2d1447a890302d9dff23ac7c8119ddce53ee65ff
        TEST_DIR test/sql
)
endif()
