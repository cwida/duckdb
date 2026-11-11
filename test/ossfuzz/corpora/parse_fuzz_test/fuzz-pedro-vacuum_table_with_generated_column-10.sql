SELECT s.min, s.max, s.has_null, s.has_no_null FROM (SELECT stats(y) s FROM test LIMIT 1);
