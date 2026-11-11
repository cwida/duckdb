SELECT * FROM t1, t0 JOIN LATERAL (SELECT t1.c1 AS col0) _subq ON true;
