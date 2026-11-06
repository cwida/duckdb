if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG e71caa1d975439f23edbd0ccc4490c114e359703
            SUBMODULES extension-ci-tools
            )
endif()
