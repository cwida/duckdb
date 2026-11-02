add_library_unity(
  test_serialization OBJECT serialization_test.cpp vector_deserialize_test.cpp
  hyperloglog_deserialize_test.cpp literal_deserialize_test.cpp)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_serialization>
    PARENT_SCOPE)
