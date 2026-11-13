add_library_unity(duckdb_operator_filter OBJECT physical_filter.cpp
                  physical_top_key_filter.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_operator_filter>
    PARENT_SCOPE)
