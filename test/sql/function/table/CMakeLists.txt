add_library_unity(
  test_table_function OBJECT table_in_out.cpp table_bind_replace.cpp
  table_declined_filter_types.cpp table_partition_info.cpp)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_table_function>
    PARENT_SCOPE)
