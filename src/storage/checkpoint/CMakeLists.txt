add_library_unity(
  duckdb_storage_checkpoint
  OBJECT
  row_group_writer.cpp
  table_data_writer.cpp
  table_index_writer.cpp
  table_data_reader.cpp
  write_overflow_strings_to_disk.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_storage_checkpoint>
    PARENT_SCOPE)
