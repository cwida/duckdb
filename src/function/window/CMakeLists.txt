add_library_unity(
  duckdb_func_window
  OBJECT
  window_aggregate_function.cpp
  window_aggregate_states.cpp
  window_aggregator.cpp
  window_boundaries_state.cpp
  window_collection.cpp
  window_constant_aggregator.cpp
  window_custom_aggregator.cpp
  window_distinct_aggregator.cpp
  window_executor.cpp
  window_index_tree.cpp
  match_recognize_matcher.cpp
  window_match_recognize.cpp
  window_merge_sort_tree.cpp
  window_naive_aggregator.cpp
  window_rank_function.cpp
  window_rownumber_function.cpp
  window_scalar_function.cpp
  window_segment_tree.cpp
  window_shared_expressions.cpp
  window_token_tree.cpp
  window_value_function.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_func_window>
    PARENT_SCOPE)
