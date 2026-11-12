if (NOT DEFINED VORTEX_ENABLED)
    set(VORTEX_ENABLED ON)
endif()
if (VORTEX_ENABLED AND NOT WIN32 AND NOT ${WASM_ENABLED} AND NOT ${MUSL_ENABLED})
    duckdb_extension_load(vortex
            GIT_URL https://github.com/vortex-data/duckdb-vortex
            GIT_TAG f56da1b2736ff2032db5c09a540303408c93f195
            APPLY_PATCHES
            LOAD_TESTS
    )
endif()
