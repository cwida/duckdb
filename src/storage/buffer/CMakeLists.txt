add_library_unity(duckdb_storage_buffer OBJECT buffer_handle.cpp
                  managed_buffer.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_storage_buffer>
    PARENT_SCOPE)
