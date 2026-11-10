if (NOT MINGW)
    duckdb_extension_load(avro
            LOAD_TESTS
            GIT_URL https://github.com/duckdb/duckdb-avro
            GIT_TAG f415fa62a7e662d200f38a30e10ae51ceb1b1f7a
            SUBMODULES "third_party/avro-c"
            APPLY_PATCHES
    )
endif()
