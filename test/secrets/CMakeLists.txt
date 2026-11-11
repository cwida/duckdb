add_library_unity(
  test_secrets OBJECT test_custom_secret_storage.cpp test_file_credentials.cpp
  test_persistent_secret_permissions.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_secrets>
    PARENT_SCOPE)
