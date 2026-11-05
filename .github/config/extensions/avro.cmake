if (NOT MINGW)
    duckdb_extension_load(avro
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-avro
            GIT_TAG 4d95356ce9433a14165156dbc6f59dcc91bd4889
            SUBMODULES "third_party/avro-c"
            APPLY_PATCHES
    )
endif()
