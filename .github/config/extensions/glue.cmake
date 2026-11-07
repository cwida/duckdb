if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG 67acf8d35df946857678f62f16f790a482fff0cf
            SUBMODULES extension-ci-tools
            )
endif()
