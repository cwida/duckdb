# Tests for the stable C++ API. The sources live next to the API itself, in
# tools/cpp/tests, so the package carries its own tests; they are compiled into
# the unittest binary from here, which is where UNITTEST_OBJECT_FILES is
# assembled.
include_directories(${CMAKE_SOURCE_DIR}/tools/cpp
                    ${CMAKE_SOURCE_DIR}/tools/cpp/tests)

# Plain add_library rather than add_library_unity: the unity helper writes
# relative #includes into a generated file, which cannot reach out of the tree.
add_library(
  test_cpp_api OBJECT
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_aggregate_function.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_appender.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_arrow.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_arrow_result.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_cast_function.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_catalog.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_column_data_collection.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_copy_function.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_core.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_custom_option.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_file_system.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_log_sink.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_query_metrics.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_pragma_function.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_prepared_statement.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_query.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_replacement_scan.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_scalar_function.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_stats.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_table_function.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_tokenizer.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_types_values.cpp
  ${CMAKE_SOURCE_DIR}/tools/cpp/tests/test_cpp_api_vector.cpp)
# The tests also call the V2 C API directly, the unstable functions included.
target_compile_definitions(test_cpp_api PRIVATE DUCKDB_V2_API_ALLOW_UNSTABLE=1)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_cpp_api>
    PARENT_SCOPE)
