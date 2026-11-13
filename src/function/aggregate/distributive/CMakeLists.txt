add_library_unity(
  duckdb_aggr_distr
  OBJECT
  count.cpp
  decimal_avg.cpp
  first_last_any.cpp
  minmax.cpp
  numeric_aggregates.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_aggr_distr>
    PARENT_SCOPE)
