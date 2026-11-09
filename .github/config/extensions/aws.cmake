if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(aws
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws
            GIT_TAG 16ae9c135300f750478d648c2496f2952a6f2827
            APPLY_PATCHES
            )
endif()
