SELECT c1 FROM ((SELECT 1 AS c1) UNION (SELECT id AS c1 FROM t0 limit 1)) A where c1 IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
