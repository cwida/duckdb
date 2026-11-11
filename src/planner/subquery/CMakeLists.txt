add_library_unity(
  duckdb_planner_subquery
  OBJECT
  delim_join_cte_rewriter.cpp
  flatten_dependent_join.cpp
  has_volatile_expressions.cpp
  pair_dependent_join.cpp
  pair_dependent_full_outer_join.cpp
  rewrite_correlated_expressions.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_planner_subquery>
    PARENT_SCOPE)
