if(NOT MINGW AND NOT ${WASM_ENABLED} AND NOT ${MUSL_ENABLED})
  duckdb_extension_load(unity_catalog
            GIT_URL https://github.com/duckdb/unity_catalog
            GIT_TAG 4cca9f7efd00930039134693e7cbd09fc4771acb
            LOAD_TESTS
  )
endif()
