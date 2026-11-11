CREATE TABLE all_types AS SELECT * EXCLUDE(small_enum, medium_enum, large_enum)
FROM test_all_types();
