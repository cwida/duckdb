# Windows tests for iceberg currently not working
IF (NOT WIN32)
  set(LOAD_ICEBERG_TESTS "LOAD_TESTS")
else ()
  set(LOAD_ICEBERG_TESTS "")
endif()
if (NOT MINGW)
  duckdb_extension_load(iceberg
     #FIXME: restore autoloading tests ${LOAD_ICEBERG_TESTS}
            GIT_URL https://github.com/duckdb/duckdb-iceberg
            GIT_TAG 4c0c0874c48ec82a3e29910554891d04bcc0e988
            )
endif()
