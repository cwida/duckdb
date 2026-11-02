if (NOT MINGW)
    duckdb_extension_load(avro
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-avro
            GIT_TAG 345c3f4e821940233b0b889e815582e5a5a33dfe
            SUBMODULES "third_party/avro-c"
            APPLY_PATCHES
    )
endif()
