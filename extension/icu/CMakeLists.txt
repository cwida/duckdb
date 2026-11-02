cmake_minimum_required(VERSION 2.8.12...3.29)

project(ICUExtension)

# Before any subdirectory or target, which would otherwise inherit the
# directory-wide definition
set_extension_version_sources(icu icu_extension.cpp)

include_directories(include)
include_directories(collation/include)
include_directories(datetime/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party/zstd/include)

set(ICU_EXTENSION_FILES
    collation/collation_collator.cpp
    collation/collation_loader.cpp
    collation/collation_normalizer.cpp
    collation/generated/collation_data.cpp
    datetime/astronomy.cpp
    datetime/calendar.cpp
    datetime/chinese.cpp
    datetime/coptic.cpp
    datetime/grego.cpp
    datetime/gregorian.cpp
    datetime/hebrew.cpp
    datetime/islamic.cpp
    datetime/japanese.cpp
    datetime/persian.cpp
    datetime/timezone.cpp
    datetime/tz_loader.cpp
    datetime/generated/tz_data.cpp
    icu_extension.cpp
    icu-current.cpp
    icu-dateadd.cpp
    icu-datefunc.cpp
    icu-datepart.cpp
    icu-datesub.cpp
    icu-datetrunc.cpp
    icu-makedate.cpp
    icu-list-range.cpp
    icu-table-range.cpp
    icu-strptime.cpp
    icu-timebucket.cpp
    icu-timezone.cpp)

build_extension_library(icu NO_WARNINGS ${ICU_EXTENSION_FILES})
link_threads(icu_extension "")
target_link_libraries(icu_extension duckdb_zstd)
target_link_libraries(icu_loadable_extension duckdb_zstd)
install(
  TARGETS icu_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
