add_library_unity(
  test_optimizer
  OBJECT
  aggregate_rewrite.cpp
  bound_function_identity.cpp
  build_probe_side_optimizer.cpp
  column_binding_rewrite.cpp
  common_subplan_nonserializable.cpp
  expression_type_serialization.cpp
  filter_pushdown_alias.cpp
  function_invocation_serialization.cpp
  join_order_conflict_detector.cpp
  logical_plan_verification.cpp
  relation_statistics.cpp
  remove_unused_columns.cpp
  sampling_pushdown_registry.cpp
  unchecked_cast.cpp
  union_alls.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_optimizer>
    PARENT_SCOPE)
