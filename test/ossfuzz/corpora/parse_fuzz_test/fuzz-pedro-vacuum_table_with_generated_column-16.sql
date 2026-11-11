SELECT s.min, s.max, s.has_null, s.has_no_null, s.approx_unique FROM (SELECT stats(x) s FROM test LIMIT 1);
