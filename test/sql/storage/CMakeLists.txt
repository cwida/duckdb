add_subdirectory(catalog)
add_library_unity(
  test_sql_storage
  OBJECT
  test_buffer_manager.cpp
  test_checksum.cpp
  test_storage.cpp
  test_database_size.cpp
  test_revert_append_segments.cpp
  wal_checkpoint_marker.cpp
  wal_prescan.cpp
  wal_torn_write.cpp)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_sql_storage>
    PARENT_SCOPE)
