set_extension_version_sources(test)

include_directories("${PROJECT_SOURCE_DIR}/third_party/catch")
include_directories("${PROJECT_SOURCE_DIR}/third_party/sqlite/include")
include_directories("${PROJECT_SOURCE_DIR}/test/include")
include_directories("${PROJECT_SOURCE_DIR}/test/extension/debug_fs/include")

add_definitions(-DDUCKDB_MAJOR_VERSION=${DUCKDB_MAJOR_VERSION})
add_definitions(-DDUCKDB_MINOR_VERSION=${DUCKDB_MINOR_VERSION})
add_definitions(-DDUCKDB_PATCH_VERSION=${DUCKDB_PATCH_VERSION})
if(DISABLE_THREADS)
  add_definitions(-DDUCKDB_NO_THREADS)
endif()

set(UNITTEST_ROOT_DIRECTORY
    ""
    CACHE
      STRING
      "The unittest root directory (defaults to the project source directory)")
if(UNITTEST_ROOT_DIRECTORY)
  set(DUCKDB_TEST_ROOT_DIRECTORY "${UNITTEST_ROOT_DIRECTORY}")
else()
  set(DUCKDB_TEST_ROOT_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

add_subdirectory("${PROJECT_SOURCE_DIR}/test/helpers"
                 "${CMAKE_BINARY_DIR}/test/helpers")
add_subdirectory("${PROJECT_SOURCE_DIR}/test/sqlite"
                 "${CMAKE_BINARY_DIR}/test/sqlite")

set(DEBUG_FS_DIRECTORY "${PROJECT_SOURCE_DIR}/test/extension/debug_fs")
add_library(
  test_debug_fs_extension OBJECT
  "${DEBUG_FS_DIRECTORY}/debug_fs_extension.cpp"
  "${DEBUG_FS_DIRECTORY}/debug_file_system.cpp"
  "${DEBUG_FS_DIRECTORY}/io_latency_model.cpp")
target_include_directories(test_debug_fs_extension
                           PRIVATE "${DEBUG_FS_DIRECTORY}/include")

build_extension_library(test HOST_BOUND test_extension.cpp
                        $<TARGET_OBJECTS:test_debug_fs_extension>)
foreach(TEST_TARGET test_extension test_loadable_extension)
  target_include_directories(
    ${TEST_TARGET}
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/test/sqlite")
  target_link_libraries(${TEST_TARGET} test_helpers test_sqlite_runner
                        duckdb_re2)
endforeach()
if(WIN32)
  link_extension_libraries(test_loadable_extension "")
endif()
