CREATE TABLE t1 (g INT, d DECIMAL(10,3), h DECIMAL(20,2), s DECIMAL(4,1));
INSERT INTO t1 VALUES (1, 1, 10, 1), (1, 2, 20, 2), (1, 3, 30, 3), (2, 10, 100, 10);
SELECT g, approx_quantile(d, 0.5)::BIGINT, approx_quantile(h, 0.5)::BIGINT, approx_quantile(s, 0.5)::BIGINT, approx_quantile(d, [0.5, 1.0]) FROM t1 GROUP BY g ORDER BY g;
