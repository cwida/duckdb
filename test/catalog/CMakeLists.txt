add_library_unity(test_catalog OBJECT test_catalog_version.cpp
                  test_duckdb_tables_projection.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_catalog>
    PARENT_SCOPE)
