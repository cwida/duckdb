if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(glue
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-aws-glue
            GIT_TAG 728ca02376b365e9c103dfe857e5de1123e9b6fe
            SUBMODULES extension-ci-tools
            APPLY_PATCHES
            )
endif()
