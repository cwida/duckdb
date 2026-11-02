if(NOT MINGW AND NOT ${WASM_ENABLED} AND NOT ${MUSL_ENABLED})
  duckdb_extension_load(unity_catalog
            GIT_URL https://github.com/duckdb/unity_catalog
            GIT_TAG cc59382f5292997293a57c2ab336c9aa97c05f2d
            LOAD_TESTS
  )
endif()
