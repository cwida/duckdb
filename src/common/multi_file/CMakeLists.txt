add_library_unity(
  duckdb_common_multi_file
  OBJECT
  base_file_reader.cpp
  multi_file_adaptive_filter_cache.cpp
  multi_file_function.cpp
  multi_file_list.cpp
  multi_file_reader.cpp
  table_function_multi_file.cpp
  multi_file_column_mapper.cpp
  union_by_name.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_common_multi_file>
    PARENT_SCOPE)
