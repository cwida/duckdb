add_library(duckdb_fmt STATIC format.cc)

target_include_directories(duckdb_fmt PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(duckdb_fmt PROPERTIES EXPORT_NAME duckdb_fmt)

install(TARGETS duckdb_fmt
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_fmt)
