include_directories(../third_party/catch)
include_directories(../third_party/sqlite/include)
include_directories(include)
include_directories(extension/debug_fs/include)

if(${DISABLE_THREADS})
  add_definitions(-DDUCKDB_NO_THREADS)
endif()

add_definitions(-DDUCKDB_MAJOR_VERSION=${DUCKDB_MAJOR_VERSION})
add_definitions(-DDUCKDB_MINOR_VERSION=${DUCKDB_MINOR_VERSION})
add_definitions(-DDUCKDB_PATCH_VERSION=${DUCKDB_PATCH_VERSION})

set(UNITTEST_OBJECT_FILES)

if(TARGET parquet_extension)
  # A static link of parquet through its generated static extension loader: the
  # describe member it names has to register the extension. Once registered
  # before main, once by the program calling duckdb_register_static_extensions
  # itself. Both are run by the Static extension link step in Main.yml.
  add_executable(static_link_smoke static_link/static_link_smoke.c)
  target_link_libraries(static_link_smoke duckdb_static)
  duckdb_link_extensions(static_link_smoke parquet)

  add_executable(static_link_explicit_smoke static_link/static_link_smoke.c)
  target_compile_definitions(static_link_explicit_smoke
                             PRIVATE STATIC_LINK_EXPLICIT)
  target_link_libraries(static_link_explicit_smoke duckdb_static)
  duckdb_link_extensions(static_link_explicit_smoke EXPLICIT parquet)
endif()

# Used by test/helpers to generate test_paths.cpp, so it needs to be set before
# adding that directory.
set(UNITTEST_ROOT_DIRECTORY
    ""
    CACHE
      STRING
      "The unittest root directory (defaults to the project source directory)")
if(DEFINED UNITTEST_ROOT_DIRECTORY AND NOT UNITTEST_ROOT_DIRECTORY STREQUAL "")
  set(DUCKDB_TEST_ROOT_DIRECTORY "${UNITTEST_ROOT_DIRECTORY}")
else()
  set(DUCKDB_TEST_ROOT_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

if(${ENABLE_UNITTEST_CPP_TESTS})
  add_subdirectory(api)
  add_subdirectory(appender)
  add_subdirectory(arrow)
  add_subdirectory(catalog)
  add_subdirectory(common)
  add_subdirectory(logging)
  add_subdirectory(memoryleak)
  add_subdirectory(secrets)
  add_subdirectory(optimizer)
  add_subdirectory(serialize)
  add_subdirectory(sql)
  add_subdirectory(sql_export)
  add_subdirectory(ossfuzz)
  add_subdirectory(mbedtls)
  add_subdirectory(encryption)
endif()
add_subdirectory(extension)
add_subdirectory(helpers)
add_subdirectory(sqlite)

if(NOT WIN32 AND NOT SUN)
  if(${ENABLE_UNITTEST_CPP_TESTS})
    add_subdirectory(persistence)
  endif()
endif()

add_executable(unittest unittest.cpp ${UNITTEST_OBJECT_FILES})

# Enable best-effort stack traces for fatal signals in the test runner without
# enabling stack traces for every DuckDB exception.
if((CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MUSL_ENABLED) OR APPLE)
  target_compile_definitions(unittest PRIVATE DUCKDB_UNITTEST_CRASH_STACKTRACE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MUSL_ENABLED)
  set_target_properties(unittest PROPERTIES ENABLE_EXPORTS ON)
endif()

set(RUN_WRAPPER_UNITTEST_NAME "unittest")
if(WIN32)
  set(RUN_WRAPPER_UNITTEST_NAME "unittest.exe")
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.py.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/run.py" @ONLY)
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.bat.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/run.bat" @ONLY)
else()
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.py.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/run" @ONLY)
  file(
    CHMOD
    "${CMAKE_CURRENT_BINARY_DIR}/run"
    PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(unittest PRIVATE -Wno-error=maybe-uninitialized)
endif()

if(TARGET duckdb AND NOT WIN32 AND NOT DUCKDB_OPENHARMONY)
  # Some tests reference internal third-party symbols (e.g. re2/mbedtls) that
  # are linked into duckdb but not exported from the shared library.
  target_link_libraries(
    unittest
    duckdb
    test_helpers
    duckdb_re2
    duckdb_mbedtls
    duckdb_fsst
    duckdb_cpp_api)
else()
  # On Windows, tests require internal DuckDB symbols that are not DLL-exported.
  target_link_libraries(unittest duckdb_static test_helpers duckdb_cpp_api)
  link_extension_libraries(unittest "")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_libraries(unittest atomic)
endif()

duckdb_codesign_for_debugging(unittest)
