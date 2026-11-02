cmake_minimum_required(VERSION 2.8.12...3.29)

project(AutoCompleteExtension)

include_directories(include)

set(AUTOCOMPLETE_EXTENSION_FILES autocomplete_extension.cpp)

build_extension_library(autocomplete ${AUTOCOMPLETE_EXTENSION_FILES})

install(
  TARGETS autocomplete_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
