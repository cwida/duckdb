add_definitions(-DDUCKDB_ROOT_DIRECTORY="${PROJECT_SOURCE_DIR}"
                -DDUCKDB_BUILD_DIRECTORY="${PROJECT_BINARY_DIR}")

# Test directories of the extensions registered with LOAD_TESTS
set(EXT_TEST_PATH_INITIALIZER "")
set(EXT_TEST_CONFIG_INITIALIZER "")
foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
  string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
  if(${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_LOAD_TESTS})
    set(EXT_TEST_PATH_INITIALIZER
        "${EXT_TEST_PATH_INITIALIZER}\t    \"${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_TEST_PATH}\",\n"
    )
    if(DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_TEST_CONFIG)
      set(EXT_TEST_CONFIG_INITIALIZER
          "${EXT_TEST_CONFIG_INITIALIZER}\t    {\"${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_TEST_PATH}\", \"${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_TEST_CONFIG}\"},\n"
      )
    endif()
  endif()
endforeach()
configure_file(extension_test_paths.cpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/extension_test_paths.cpp" @ONLY)

set(DUCKDB_TEST_HELPERS_UNITS
    test_helpers.cpp capi_tester.cpp pid.cpp test_config.cpp test_reporter.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/extension_test_paths.cpp")

add_library(test_helpers STATIC ${DUCKDB_TEST_HELPERS_UNITS})
