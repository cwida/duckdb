add_library_unity(
  test_sql_export
  OBJECT
  bound_expression_sql_export_test_helpers.cpp
  logical_plan_sql_export_test_helpers.cpp
  portable_plan_divergence.cpp
  sql_export_expression_aggregates.cpp
  sql_export_expression_functions.cpp
  sql_export_expression_literals.cpp
  sql_export_expression_structure.cpp
  sql_export_plan_chunks.cpp
  sql_export_plan_cte_limit.cpp
  sql_export_plan_extensions.cpp
  sql_export_plan_joins.cpp
  sql_export_plan_pivot.cpp
  sql_export_plan_relations.cpp
  sql_export_plan_sources.cpp
  sql_export_plan_statements.cpp
  sql_export_plan_values.cpp
  sql_export_row_id_selection.cpp
  sql_export_statement_verification.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_sql_export>
    PARENT_SCOPE)
