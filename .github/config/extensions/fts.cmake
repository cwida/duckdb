duckdb_extension_load(fts
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-fts
        GIT_TAG 2266e67d01c737cb65114e77c7c672e58869a635
        TEST_DIR test/sql
        APPLY_PATCHES
)
