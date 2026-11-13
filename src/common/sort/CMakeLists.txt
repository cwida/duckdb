add_library_unity(
  duckdb_sort
  OBJECT
  full_sort.cpp
  hashed_sort.cpp
  natural_sort.cpp
  partition_key_tracker.cpp
  partitioned_sort.cpp
  sort.cpp
  sort_strategy.cpp
  sorted_run.cpp
  sorted_run_merger.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_sort>
    PARENT_SCOPE)
