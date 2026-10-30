if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MUSL_ENABLED)
    return()
endif()

duckdb_extension_load(sirius
    GIT_URL https://github.com/sirius-db/sirius
    GIT_TAG 62a1535916fd7006c6ecfd2288cfebeff78b56a4
    SOURCE_DIR sirius-duckdb/loader
    INCLUDE_DIR sirius-duckdb/loader/src
    SUBMODULES ""
)
