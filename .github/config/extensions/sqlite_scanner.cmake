# Static linking on windows does not properly work due to symbol collision
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG c8bfee013e2fa88a24a2ba0a30f1fcc129031207
        SUBMODULES database-connector
        )
