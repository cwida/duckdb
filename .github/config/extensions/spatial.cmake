if (${BUILD_COMPLETE_EXTENSION_SET})
################# SPATIAL
duckdb_extension_load(spatial
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-spatial
    GIT_TAG a3dbcd1781ec26c3a1be9bd622be05b376d6f47a
    INCLUDE_DIR src/spatial
    TEST_DIR test/sql
    APPLY_PATCHES
    )
endif()
