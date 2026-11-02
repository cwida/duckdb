add_library_unity(duckdb_core_functions_variant OBJECT variant_group_object.cpp)
set(CORE_FUNCTION_FILES
    ${CORE_FUNCTION_FILES} $<TARGET_OBJECTS:duckdb_core_functions_variant>
    PARENT_SCOPE)
