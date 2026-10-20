add_subdirectory(dependency)

add_library_unity(
  duckdb_catalog_entries
  OBJECT
  aggregate_function_catalog_entry.cpp
  copy_function_catalog_entry.cpp
  duck_index_entry.cpp
  duck_schema_entry.cpp
  duck_table_entry.cpp
  type_catalog_entry.cpp
  index_catalog_entry.cpp
  language_catalog_entry.cpp
  macro_catalog_entry.cpp
  pragma_function_catalog_entry.cpp
  schema_catalog_entry.cpp
  sequence_catalog_entry.cpp
  table_catalog_entry.cpp
  trigger_catalog_entry.cpp
  column_dependency_manager.cpp
  scalar_function_catalog_entry.cpp
  table_function_catalog_entry.cpp
  view_catalog_entry.cpp
  window_function_catalog_entry.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_catalog_entries>
    PARENT_SCOPE)
