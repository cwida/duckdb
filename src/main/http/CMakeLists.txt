add_library_unity(
  duckdb_common_http
  OBJECT
  http_client_pool.cpp
  http_retry_budget.cpp
  http_transport_config.cpp
  http_transport_manager.cpp
  http_util.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_common_http>
    PARENT_SCOPE)

# The built-in httplib client is its own archive, registered like a static
# extension through duckdb_extension_httplib_describe
if(ENABLE_BUILTIN_HTTPLIB)
  add_library(duckdb_httplib STATIC http_client_httplib.cpp)
  target_include_directories(duckdb_httplib
                             PRIVATE ${PROJECT_SOURCE_DIR}/third_party/httplib)
  target_link_libraries(duckdb_httplib duckdb_static)
  duckdb_make_native_lto_archive(duckdb_httplib)
  install(
    TARGETS duckdb_httplib
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
endif()
