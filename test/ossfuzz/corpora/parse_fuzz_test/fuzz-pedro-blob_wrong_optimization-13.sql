SELECT 1 FROM t1 WHERE NOT (decode('\xC0'::BLOB) IS NULL); --1 row;
