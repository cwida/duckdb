if (NOT MINGW)
  set(ICEBERG_TEST_OPTIONS)
  if (BUILD_UNITTESTS AND NOT EMSCRIPTEN AND NOT EXTENSION_TESTS_ONLY)
    set(ICEBERG_TEST_OPTIONS
        LOAD_TESTS
        TEST_DIR test/sql/local/native_catalog
        TEST_CONFIG test/configs/mock_native.json)
  endif()
  duckdb_extension_load(iceberg
            ${ICEBERG_TEST_OPTIONS}
            GIT_URL https://github.com/duckdb/duckdb-iceberg
            GIT_TAG faed61aae7c26da8537c7c110a7e603828b08d67
            APPLY_PATCHES
            )
  if (DUCKDB_EXTENSION_ICEBERG_LOAD_TESTS)
    # The native credential tests share Iceberg's C++ callbacks with unittest.
    # Avro and Parquet must be registered before Iceberg's startup loader.
    duckdb_extension_statically_link(avro parquet iceberg iceberg_mock)
    # Cloud CI publishes all loadables; keep the mock in the test binary.
    set(ICEBERG_MOCK_BUILD_LOADABLE OFF)
    include("${DUCKDB_EXTENSION_ICEBERG_PATH}/test/native_catalog/extension_config.cmake")
  endif()
  unset(ICEBERG_TEST_OPTIONS)
endif()
