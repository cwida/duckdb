add_library_unity(
  duckdb_common_variant
  OBJECT
  variant.cpp
  variant_comparison.cpp
  variant_iterator.cpp
  variant_value_convert.cpp
  variant_binary_decoder.cpp
  parquet_variant_iterator.cpp
  parquet_variant_encoding.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_common_variant>
    PARENT_SCOPE)
