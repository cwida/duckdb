add_library_unity(
  duckdb_func_system
  OBJECT
  aggregate_export.cpp
  write_log.cpp
  current_transaction_id.cpp
  current_connection_id.cpp
  current_query_id.cpp
  parse_log_message.cpp
  last_commit.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_func_system>
    PARENT_SCOPE)
