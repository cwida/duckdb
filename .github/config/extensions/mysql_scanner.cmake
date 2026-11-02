if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(mysql_scanner
            GIT_URL https://github.com/duckdb/duckdb-mysql
            GIT_TAG f7fc0d42716534aa5ad48626d21ced9807b33967
            SUBMODULES database-connector
            )
endif()
