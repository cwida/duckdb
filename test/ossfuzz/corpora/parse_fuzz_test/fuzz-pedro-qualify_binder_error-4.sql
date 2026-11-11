SELECT 1 FROM (SELECT 2) t0(c0) QUALIFY (c0, dense_rank() OVER(), mode(0));
