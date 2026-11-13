find_package(Threads REQUIRED)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_library(duckdb_fsst STATIC libfsst.cpp)

target_include_directories(duckdb_fsst PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(duckdb_fsst PROPERTIES EXPORT_NAME duckdb_fsst)

install(TARGETS duckdb_fsst
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_fsst)