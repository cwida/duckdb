add_library_unity(
  duckdb_planner_sql_export
  OBJECT
  bound_expression_sql_exporter.cpp
  logical_plan_sql_exporter.cpp
  sql_export_composition.cpp
  sql_export_constants.cpp
  sql_export_cte.cpp
  sql_export_dml.cpp
  sql_export_functions.cpp
  sql_export_joins.cpp
  sql_export_limit.cpp
  sql_export_pivot.cpp
  sql_export_relations.cpp
  sql_export_scope.cpp
  sql_export_sources.cpp
  sql_export_statements.cpp
  sql_export_values.cpp
  sql_export_window.cpp
  table_function_sql_export.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_planner_sql_export>
    PARENT_SCOPE)
