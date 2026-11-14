if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MUSL_ENABLED)
    return()
endif()

duckdb_extension_load(sirius
    GIT_URL https://github.com/sirius-db/sirius
    GIT_TAG 83d6c0ee01f2a98b97ce04c74141e2881f49438e
    SOURCE_DIR sirius-duckdb/loader
    INCLUDE_DIR sirius-duckdb/loader/src
    SUBMODULES ""
)
