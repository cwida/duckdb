add_library_unity(
  duckdb_optimizer_relation_statistics
  OBJECT
  relation_statistics.cpp
  relation_statistics_extractor.cpp
  relation_statistics_helper.cpp
  relation_statistics_operator.cpp
  relation_statistics_sample.cpp
  relation_statistics_scan.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_relation_statistics>
    PARENT_SCOPE)
