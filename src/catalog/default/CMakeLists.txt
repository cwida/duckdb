add_library_unity(
  duckdb_catalog_default_entries
  OBJECT
  default_coordinate_systems.cpp
  default_functions.cpp
  default_generator.cpp
  default_macro_qualifier.cpp
  default_schemas.cpp
  default_table_functions.cpp
  default_types.cpp
  default_views.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_catalog_default_entries>
    PARENT_SCOPE)
