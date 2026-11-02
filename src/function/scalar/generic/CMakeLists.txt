add_library_unity(
  duckdb_func_generic_main
  OBJECT
  barrier.cpp
  constant_or_null.cpp
  error.cpp
  getvariable.cpp
  has_error.cpp
  invoke.cpp
  try_rethrow.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_func_generic_main>
    PARENT_SCOPE)
