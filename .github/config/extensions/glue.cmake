if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG 3d239f60cb6b786f656909aca33141040ed3fb20
            SUBMODULES extension-ci-tools
            )
endif()
