# Stable C++ API, built on top of DuckDBs C-API-V2. This is compiled into
# consumers, never into libduckdb. Hidden visibility keeps duckdb_api symbols
# out of the export surface of anything this archive ends up linked into (e.g.
# via a static extension that reaches the shared library).

add_library(duckdb_cpp_api STATIC duckdb_cpp.cpp)
target_include_directories(
  duckdb_cpp_api PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_cpp_api PROPERTIES CXX_VISIBILITY_PRESET hidden
                                                VISIBILITY_INLINES_HIDDEN ON)

# Loadable-extension flavor: every duckdb_v2_* call is routed through the
# extension vtable populated by the entrypoint, so a dynamically loaded
# extension linking this archive carries no undefined engine symbols. The
# unstable guard exposes the full API surface, but requires the extension to be
# pinned and built against a specific DuckDB version (similarly to how raw C++
# extensions that link into the core work)

add_library(duckdb_cpp_api_loadable STATIC duckdb_cpp.cpp)
target_include_directories(duckdb_cpp_api_loadable
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(
  duckdb_cpp_api_loadable
  PRIVATE DUCKDB_CPP_API_LOADABLE
          DUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_NORMALIZED_VERSION})

set_target_properties(
  duckdb_cpp_api_loadable PROPERTIES CXX_VISIBILITY_PRESET hidden
                                     VISIBILITY_INLINES_HIDDEN ON)

# Statically linked extensions written against the C++ API link the archive, so
# it is part of the export set
install(
  TARGETS duckdb_cpp_api
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
