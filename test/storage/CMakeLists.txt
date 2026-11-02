add_library_unity(test_storage OBJECT alter_type_committed_update_race.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_storage>
    PARENT_SCOPE)
