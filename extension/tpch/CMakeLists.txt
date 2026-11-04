cmake_minimum_required(VERSION 2.8.12...3.29)

project(TpchExtension)

# Before any subdirectory or target, which would otherwise inherit the
# directory-wide definition
set_extension_version_sources(tpch tpch_extension.cpp)

include_directories(dbgen/include)
include_directories(include)
add_subdirectory(dbgen)

# test/api reaches TPC internals through the shared libduckdb
build_extension_library(tpch DEFAULT_VISIBILITY tpch_extension.cpp
                        ${DBGEN_OBJECT_FILES})

install(
  TARGETS tpch_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
