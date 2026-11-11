if(NOT MINGW AND NOT ${WASM_ENABLED})
  duckdb_extension_load(delta
            GIT_URL https://github.com/duckdb/duckdb-delta
            GIT_TAG f41762b37b0e14f742a46f58ba59c2e656e08812
            SUBMODULES extension-ci-tools
  )
endif()
