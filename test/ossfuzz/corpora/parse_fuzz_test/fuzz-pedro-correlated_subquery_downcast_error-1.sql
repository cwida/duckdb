SELECT count(*) OVER() = ANY(SELECT * FROM t0 t1 WHERE(c0 = t0.c0)) FROM t0;
