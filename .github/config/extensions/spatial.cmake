if (${BUILD_COMPLETE_EXTENSION_SET})
################# SPATIAL
duckdb_extension_load(spatial
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-spatial
    GIT_TAG e3b4a07a159b18e9fd15a68910c07f203bc357cf
    INCLUDE_DIR src/spatial
    TEST_DIR test/sql
    )
endif()
