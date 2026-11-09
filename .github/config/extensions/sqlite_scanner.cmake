# Static linking on windows does not properly work due to symbol collision
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG 7a09f94cb58d30e78c594f3e55a35b2def58a3e9
        SUBMODULES database-connector
        )
