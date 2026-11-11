if (NOT DEFINED VORTEX_ENABLED)
    set(VORTEX_ENABLED ON)
endif()
if (VORTEX_ENABLED AND NOT MINGW AND NOT ${WASM_ENABLED} AND NOT ${MUSL_ENABLED})
    duckdb_extension_load(vortex
            GIT_URL https://github.com/vortex-data/duckdb-vortex
            GIT_TAG 89091fc7dfa8b5e924b2596be65e2a91d6778ef5
            APPLY_PATCHES
            LOAD_TESTS
    )
endif()
