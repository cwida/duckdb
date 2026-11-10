add_library_unity(
  duckdb_query_node
  OBJECT
  recursive_cte_node.cpp
  trampoline_cte_node.cpp
  cte_node.cpp
  select_node.cpp
  set_operation_node.cpp
  statement_node.cpp
  update_query_node.cpp
  delete_query_node.cpp
  insert_query_node.cpp
  merge_query_node.cpp
  copy_query_node.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_query_node>
    PARENT_SCOPE)
