# Static linking on windows does not properly work due to symbol collision
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG dafb4c2f3f8615f9c18c31304c00ca4bdd43afb0
        SUBMODULES database-connector
        )
