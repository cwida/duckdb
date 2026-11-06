if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG 0e1232870b8215abd21c5d1483d00375059c47e0
            SUBMODULES extension-ci-tools
            )
endif()
