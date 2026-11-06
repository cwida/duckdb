if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG ded99c8b9ae6fce51ea5992a91744099458801f3
            SUBMODULES extension-ci-tools
            )
endif()
