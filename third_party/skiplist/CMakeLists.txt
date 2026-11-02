add_library(duckdb_skiplistlib STATIC SkipList.cpp)

target_include_directories(
  duckdb_skiplistlib
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_skiplistlib PROPERTIES EXPORT_NAME duckdb_skiplistlib)

install(TARGETS duckdb_skiplistlib
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_skiplistlib)
