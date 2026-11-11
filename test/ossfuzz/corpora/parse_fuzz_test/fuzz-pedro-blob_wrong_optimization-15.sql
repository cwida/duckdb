SELECT 1 FROM t1 WHERE (decode('\xC0'::BLOB) IS NULL) IS NULL; --1 row, wrong?;
