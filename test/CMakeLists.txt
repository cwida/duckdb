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

set(TEST_EXTENSION_PARAMETERS "-warnings -host-bound")
if(WIN32)
  set(TEST_EXTENSION_PARAMETERS "-warnings")
endif()
build_loadable_extension_directory(
  test
  "CPP"
  extension/test
  "default-version"
  ""
  "${TEST_EXTENSION_PARAMETERS}"
  "${PROJECT_SOURCE_DIR}/extension/test/test_extension.cpp"
  $<TARGET_OBJECTS:test_debug_fs_extension>)
set_target_properties(
  test_loadable_extension
  PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension/test"
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension/test")
target_include_directories(
  test_loadable_extension
  PRIVATE "${PROJECT_SOURCE_DIR}/extension/test/include"
          "${PROJECT_SOURCE_DIR}/extension/test"
          "${PROJECT_SOURCE_DIR}/test/sqlite"
          "${PROJECT_SOURCE_DIR}/test/extension/debug_fs/include")
target_link_libraries(test_loadable_extension test_helpers test_sqlite_runner
                      duckdb_re2)
set_target_properties(shell PROPERTIES ENABLE_EXPORTS ON)

if(${ENABLE_UNITTEST_CPP_TESTS})
  duckdb_add_test_bundle(duckdb_tests SOURCES ${UNITTEST_OBJECT_FILES})
  target_compile_definitions(duckdb_tests_loadable_extension
                             PRIVATE DUCKDB_TEST_BUNDLE_DEBUG_FS)
  if((CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MUSL_ENABLED) OR APPLE)
    target_compile_definitions(duckdb_tests_loadable_extension
                               PRIVATE DUCKDB_UNITTEST_CRASH_STACKTRACE)
  endif()
  target_include_directories(
    duckdb_tests_loadable_extension
    PRIVATE "${PROJECT_SOURCE_DIR}/test/extension/debug_fs/include")
  target_link_libraries(duckdb_tests_loadable_extension duckdb_fsst
                        duckdb_mbedtls)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT CMAKE_CROSSCOMPILING
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND ENABLE_UNITTEST_CPP_TESTS
   AND TARGET duckdb)
  # Exercise real allocator failures without installing hooks in the library.
  add_executable(
    block_allocator_fault_injection api/block_allocator_fault_injection.cpp
                                    ../src/storage/block_allocator.cpp)
  target_link_libraries(block_allocator_fault_injection duckdb pthread)
  target_link_options(
    block_allocator_fault_injection
    PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=_Znwm
    -Wl,--wrap=_ZdlPv
    -Wl,--wrap=_ZdlPvm
    -Wl,--wrap=mmap
    -Wl,--wrap=mprotect
    -Wl,--wrap=munmap)
  set_target_properties(block_allocator_fault_injection
                        PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE)
  target_sources(duckdb_tests_loadable_extension
                 PRIVATE api/test_block_allocator_fault_injection.cpp)
  add_dependencies(duckdb_tests_loadable_extension block_allocator_fault_injection)
endif()

set(RUN_WRAPPER_DUCKDB_NAME "duckdb")
set(RUN_WRAPPER_TEST_ROOTS "")
foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
  string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
  if(${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_LOAD_TESTS})
    list(APPEND RUN_WRAPPER_TEST_ROOTS
         "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_TEST_PATH}")
  endif()
endforeach()
set(RUN_WRAPPER_DEFAULT_ARGS "")
if(WIN32)
  set(RUN_WRAPPER_DUCKDB_NAME "duckdb.exe")
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.py.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/run.py" @ONLY)
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.bat.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/run.bat" @ONLY)
  set(RUN_WRAPPER_DEFAULT_ARGS "--workers;1")
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.py.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/unittest.py" @ONLY)
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_unittest_wrapper.bat.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/unittest.bat" @ONLY)
else()
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.py.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/run" @ONLY)
  set(RUN_WRAPPER_DEFAULT_ARGS "--workers;1")
  configure_file("${PROJECT_SOURCE_DIR}/scripts/ci/test_runner_wrapper.py.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/unittest" @ONLY)
  foreach(RUN_WRAPPER_NAME run unittest)
    file(
      CHMOD
      "${CMAKE_CURRENT_BINARY_DIR}/${RUN_WRAPPER_NAME}"
      PERMISSIONS
      OWNER_READ
      OWNER_WRITE
      OWNER_EXECUTE
      GROUP_READ
      GROUP_EXECUTE
      WORLD_READ
      WORLD_EXECUTE)
  endforeach()
endif()
