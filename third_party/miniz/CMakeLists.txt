add_library(duckdb_miniz STATIC miniz.cpp)

target_include_directories(
  duckdb_miniz
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_miniz PROPERTIES EXPORT_NAME duckdb_duckdb_miniz)

install(TARGETS duckdb_miniz
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_miniz)
