if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG 8b310c81f227cf7630fd4ff52925f208f0d35278
            SUBMODULES extension-ci-tools
            )
endif()
