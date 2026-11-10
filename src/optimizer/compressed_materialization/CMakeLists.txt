add_library_unity(
  duckdb_optimizer_compressed_materialization OBJECT compress_aggregate.cpp
  compress_comparison_join.cpp compress_order.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES}
    $<TARGET_OBJECTS:duckdb_optimizer_compressed_materialization>
    PARENT_SCOPE)
