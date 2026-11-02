# Test directories of the extensions registered with LOAD_TESTS, and the build
# and root directory. These are only put into this generated file, instead of
# being defines for the test sources, so that only this file depends on where
# the checkout is. That lets compiler caches like ccache share the other objects
# between checkouts, e.g. git worktrees.
set(EXT_TEST_PATH_INITIALIZER "")
foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
  string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
  if(${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_LOAD_TESTS})
    set(EXT_TEST_PATH_INITIALIZER
        "${EXT_TEST_PATH_INITIALIZER}\t    \"${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_TEST_PATH}\",\n"
    )
  endif()
endforeach()
configure_file(test_paths.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/test_paths.cpp"
               @ONLY)

set(DUCKDB_TEST_HELPERS_UNITS
    test_helpers.cpp capi_tester.cpp pid.cpp test_config.cpp test_reporter.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/test_paths.cpp")

add_library(test_helpers STATIC ${DUCKDB_TEST_HELPERS_UNITS})
