add_library(duckdb_hyperloglog STATIC hyperloglog.cpp sds.cpp)

target_include_directories(
  duckdb_hyperloglog
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_hyperloglog PROPERTIES EXPORT_NAME duckdb_hyperloglog)

install(TARGETS duckdb_hyperloglog
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_hyperloglog)