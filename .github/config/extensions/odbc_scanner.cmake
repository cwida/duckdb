if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(odbc_scanner
            GIT_URL https://github.com/duckdb/odbc-scanner
            GIT_TAG ef4382fbfcb238e3012373384ae4e1953f3736a3
            )
endif()
