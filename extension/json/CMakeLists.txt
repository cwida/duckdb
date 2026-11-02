cmake_minimum_required(VERSION 2.8.12...3.29)

project(JSONExtension)

# Before any subdirectory or target, which would otherwise inherit the
# directory-wide definition
set_extension_version_sources(json json_extension.cpp)

include_directories(include)
add_subdirectory(json_functions)

set(JSON_EXTENSION_FILES
    json_reader.cpp
    json_extension.cpp
    json_common.cpp
    json_enums.cpp
    json_functions.cpp
    json_scan.cpp
    json_schema_inference.cpp
    json_serializer.cpp
    json_deserializer.cpp
    serialize_json.cpp
    json_functions/copy_json.cpp
    json_functions/json_array_length.cpp
    json_functions/json_contains.cpp
    json_functions/json_exists.cpp
    json_functions/json_extract.cpp
    json_functions/json_keys.cpp
    json_functions/json_merge_patch.cpp
    json_functions/json_pretty.cpp
    json_functions/json_table_in_out.cpp
    json_functions/json_structure.cpp
    json_functions/json_transform.cpp
    json_functions/json_create.cpp
    json_functions/json_geojson.cpp
    json_functions/json_type.cpp
    json_functions/json_valid.cpp
    json_functions/json_value.cpp
    json_functions/json_serialize_sql.cpp
    json_functions/json_serialize_plan.cpp
    json_functions/json_normalize.cpp
    json_functions/json_deep_merge.cpp
    json_functions/json_merge_patch_diff.cpp
    json_functions/json_modify.cpp
    json_functions/json_strip_nulls.cpp
    json_functions/read_json.cpp
    json_functions/read_single_json_file.cpp)

build_extension_library(json ${JSON_EXTENSION_FILES})
target_link_libraries(json_loadable_extension duckdb_yyjson)

install(
  TARGETS json_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
