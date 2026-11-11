SELECT 1 FROM t1 WHERE decode('\xC0'::BLOB) IS NULL; --1 row, wrong?;
