set(PARAMETERS "-warnings")

set(DEBUG_FS_EXTENSION_FILES
    debug_fs/debug_fs_extension.cpp debug_fs/debug_file_system.cpp
    debug_fs/io_latency_model.cpp)
set(DEBUG_FS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/debug_fs/include")

build_loadable_extension_directory(
  debug_fs
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  ${DEBUG_FS_EXTENSION_FILES})
target_include_directories(debug_fs_loadable_extension
                           PRIVATE ${DEBUG_FS_INCLUDE_DIR})

add_library(test_debug_fs_extension OBJECT ${DEBUG_FS_EXTENSION_FILES})
target_include_directories(test_debug_fs_extension
                           PRIVATE ${DEBUG_FS_INCLUDE_DIR})
list(APPEND UNITTEST_OBJECT_FILES $<TARGET_OBJECTS:test_debug_fs_extension>)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES}
    PARENT_SCOPE)

build_loadable_extension_directory(
  loadable_extension_demo
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  loadable_extension_demo.cpp)

build_loadable_extension_directory(
  loadable_grammar_extension_demo
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  loadable_grammar_extension_demo.cpp)

build_loadable_extension_directory(
  loadable_dialect_extension_demo
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  loadable_dialect_extension_demo.cpp)

build_loadable_extension_directory(
  explicit_extension_schema
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  explicit_extension_schema.cpp)

build_loadable_extension_directory(
  explicit_extension_schema_alias
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  explicit_extension_schema_alias.cpp)

build_loadable_extension_directory(
  loadable_extension_demo_double
  "CPP"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  loadable_extension_demo_alt.cpp)

# A V2 C API extension, written against the stable C++ API. C_STRUCT_UNSTABLE
# pins it to this exact DuckDB build, which is also what selects the V2
# entrypoint at load time.
build_loadable_extension_directory(
  cpp_api_demo
  "C_STRUCT_UNSTABLE"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  cpp_api_demo.cpp)
target_compile_definitions(
  cpp_api_demo_loadable_extension
  PRIVATE DUCKDB_EXTENSION_NAME=cpp_api_demo DUCKDB_CPP_API_LOADABLE
          DUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_NORMALIZED_VERSION})
target_include_directories(cpp_api_demo_loadable_extension
                           PRIVATE ${CMAKE_SOURCE_DIR}/tools/cpp)
target_link_libraries(cpp_api_demo_loadable_extension duckdb_cpp_api_loadable)

# Same, but its entrypoint throws: covers the error slot aborting a load.
build_loadable_extension_directory(
  cpp_api_demo_fail
  "C_STRUCT_UNSTABLE"
  test/extension
  "default-version"
  ""
  ${PARAMETERS}
  cpp_api_demo_fail.cpp)
target_compile_definitions(
  cpp_api_demo_fail_loadable_extension
  PRIVATE DUCKDB_EXTENSION_NAME=cpp_api_demo_fail DUCKDB_CPP_API_LOADABLE
          DUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_NORMALIZED_VERSION})
target_include_directories(cpp_api_demo_fail_loadable_extension
                           PRIVATE ${CMAKE_SOURCE_DIR}/tools/cpp)
target_link_libraries(cpp_api_demo_fail_loadable_extension
                      duckdb_cpp_api_loadable)

if(NOT WIN32 AND NOT SUN)
  build_loadable_extension_directory(
    loadable_extension_optimizer_demo
    "CPP"
    test/extension
    "default-version"
    ""
    ${PARAMETERS}
    ../extension/loadable_extension_optimizer_demo.cpp)

  if(${ENABLE_UNITTEST_CPP_TESTS} AND NOT (${ENABLE_THREAD_SANITIZER}))
    set(TEST_EXT_OBJECTS test_remote_optimizer.cpp)

    add_library_unity(test_extensions OBJECT ${TEST_EXT_OBJECTS})
    set(UNITTEST_OBJECT_FILES
        ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_extensions>
        PARENT_SCOPE)
  endif()
endif()
