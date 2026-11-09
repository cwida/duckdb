add_library_unity(
  duckdb_operator_set
  OBJECT
  physical_union.cpp
  physical_recursive_cte.cpp
  physical_recursive_cte_delta.cpp
  physical_recursive_cte_keyed.cpp
  physical_recursive_cte_runtime.cpp
  physical_recursive_cte_state.cpp
  physical_recursive_cte_union.cpp
  physical_cte.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_operator_set>
    PARENT_SCOPE)
