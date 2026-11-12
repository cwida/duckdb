if (${BUILD_COMPLETE_EXTENSION_SET})
################# SPATIAL
duckdb_extension_load(spatial
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-spatial
    GIT_TAG 37bff11aea111375eca0acad7b37a7ac41896af9
    INCLUDE_DIR src/spatial
    TEST_DIR test/sql
    )
endif()
