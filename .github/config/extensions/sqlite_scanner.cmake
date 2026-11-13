# Static linking on windows does not properly work due to symbol collision
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG ce0987fda4f6393231c6783ee32f72ab873a6d3e
        APPLY_PATCHES
        SUBMODULES database-connector
        )
