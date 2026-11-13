add_library_unity(
  duckdb_logging
  OBJECT
  log_manager.cpp
  log_sink.cpp
  log_types.cpp
  logger.cpp
  logging.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_logging>
    PARENT_SCOPE)
