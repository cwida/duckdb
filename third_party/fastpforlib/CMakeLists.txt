add_library(duckdb_fastpforlib STATIC bitpacking.cpp)

target_include_directories(
        duckdb_fastpforlib
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_fastpforlib PROPERTIES EXPORT_NAME duckdb_fastpforlib)

install(TARGETS duckdb_fastpforlib
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_fastpforlib)