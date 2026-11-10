add_library_unity(
  duckdb_common_types_row
  OBJECT
  integer_partitioned_tuple_data.cpp
  partitioned_tuple_data.cpp
  tuple_data_allocator.cpp
  tuple_data_collection.cpp
  tuple_data_iterator.cpp
  tuple_data_layout.cpp
  tuple_data_scatter_gather.cpp
  tuple_data_segment.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_common_types_row>
    PARENT_SCOPE)
