add_library_unity(
  duckdb_row_operations
  OBJECT
  row_aggregate.cpp
  row_scatter.cpp
  row_gather.cpp
  row_match.cpp
  row_external.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_row_operations>
    PARENT_SCOPE)
