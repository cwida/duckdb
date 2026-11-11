duckdb_extension_load(vss
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-vss
        GIT_TAG 3685131eacaf0d467d430be5c4e5e7d1d483437a
        TEST_DIR test/sql
        APPLY_PATCHES
    )
