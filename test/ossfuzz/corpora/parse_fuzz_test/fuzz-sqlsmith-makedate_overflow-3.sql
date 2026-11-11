SELECT make_date(c1) 
FROM test_vector_types(CAST(NULL AS STRUCT("year" BIGINT, "month" BIGINT, "day" BIGINT))) AS test_vector_types(c1);
