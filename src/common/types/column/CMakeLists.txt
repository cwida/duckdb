add_library_unity(
  duckdb_common_types_column
  OBJECT
  column_data_allocator.cpp
  column_data_collection.cpp
  column_data_collection_segment.cpp
  column_data_consumer.cpp
  partitioned_column_data.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_common_types_column>
    PARENT_SCOPE)
