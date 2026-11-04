if(NOT MINGW AND NOT ${WASM_ENABLED})
  duckdb_extension_load(delta
            GIT_URL https://github.com/duckdb/duckdb-delta
            GIT_TAG 9dcfb65b99fcafaa068222c2490abc143da4cc8e
            SUBMODULES extension-ci-tools
  )
endif()
