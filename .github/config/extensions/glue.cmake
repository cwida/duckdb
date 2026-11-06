if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG 15e751a9a4ca525e3ede2bb47205f2500425de2c
            SUBMODULES extension-ci-tools
            )
endif()
