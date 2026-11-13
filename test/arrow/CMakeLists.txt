add_library_unity(
  test_arrow_roundtrip
  OBJECT
  arrow_test_helper.cpp
  arrow_roundtrip.cpp
  arrow_move_children.cpp
  arrow_filter_pushdown.cpp
  arrow_dictionary_sentinel.cpp
  arrow_dict_nested_offset.cpp
  arrow_list_view_nested_offset.cpp
  arrow_nested_struct_offset.cpp
  arrow_output_version_buffers.cpp
  arrow_run_end_encoded.cpp
  arrow_fixed_size_list_null_children.cpp
  arrow_union_offset.cpp)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_arrow_roundtrip>
    PARENT_SCOPE)
