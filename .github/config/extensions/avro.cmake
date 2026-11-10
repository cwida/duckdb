if (NOT MINGW)
    duckdb_extension_load(avro
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-avro
            GIT_TAG b7b157f50dd0a3e6cfd30d9dd273a388fc2b9939
            SUBMODULES "third_party/avro-c"
            APPLY_PATCHES
    )
endif()
