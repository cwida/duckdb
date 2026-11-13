if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(mysql_scanner
            GIT_URL https://github.com/duckdb/duckdb-mysql
            GIT_TAG 85d46244386d4d8db73a3872f75584f58d644b75
            APPLY_PATCHES
            SUBMODULES database-connector
            )
endif()
