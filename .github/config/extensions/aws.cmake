if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(aws
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws
            GIT_TAG ff4ff9508813ce97b173d7d0cf8ae9f5640a7d4f
            )
endif()
