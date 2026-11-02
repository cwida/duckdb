add_library_unity(
  duckdb_operator_csv_table_function OBJECT csv_file_scanner.cpp
  global_csv_state.cpp csv_schema_discovery.cpp read_single_csv_file.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_operator_csv_table_function>
    PARENT_SCOPE)
