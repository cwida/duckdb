# lance cannot write datasets to absolute Windows paths yet, so its tests fail on Windows
if (NOT WIN32)
    set(LOAD_LANCE_TESTS "LOAD_TESTS")
else()
    set(LOAD_LANCE_TESTS "")
endif()
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(lance
            GIT_URL https://github.com/lance-format/lance-duckdb
            GIT_TAG 86118537da79138e6ec5c7358f2384c7ae650ab0
            SUBMODULES extension-ci-tools
            ${LOAD_LANCE_TESTS}
    )
endif()
