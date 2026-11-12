add_library_unity(
  test_sql_interquery_parallelism OBJECT concurrent_attach_detach.cpp
  concurrent_detach_released_reference.cpp test_concurrent_prepared.cpp)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_sql_interquery_parallelism>
    PARENT_SCOPE)
