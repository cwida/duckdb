# Static linking on windows does not properly work due to symbol collision
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG 695e1cc2d8389c40a3d5d2fbe9a968d19e6b34be
        SUBMODULES database-connector
        )
