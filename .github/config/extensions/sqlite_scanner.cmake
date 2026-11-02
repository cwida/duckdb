# Static linking on windows does not properly work due to symbol collision
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG e223dc01493414cef6d5960744f1e048d59fc592
        SUBMODULES database-connector
        )
