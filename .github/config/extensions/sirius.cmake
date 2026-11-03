if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MUSL_ENABLED)
    return()
endif()

duckdb_extension_load(sirius
    GIT_URL https://github.com/sirius-db/sirius
    GIT_TAG a1d2f068922b26df3da913672503d2e9cc8e4bd5
    SOURCE_DIR sirius-duckdb/loader
    INCLUDE_DIR sirius-duckdb/loader/src
    SUBMODULES ""
)
