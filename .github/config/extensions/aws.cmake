if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(aws
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws
            GIT_TAG c2f2568a683d4159a9d976f765b7e6b7577d17c7
            APPLY_PATCHES
            )
endif()
