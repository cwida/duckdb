add_library(duckdb_yyjson STATIC yyjson.cpp)

target_include_directories(
        duckdb_yyjson
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(duckdb_yyjson PROPERTIES EXPORT_NAME duckdb_duckdb_yyjson)

install(TARGETS duckdb_yyjson
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_yyjson)
