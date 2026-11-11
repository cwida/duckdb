SELECT 1 FROM (SELECT 2) t0(c0) QUALIFY (count(sum(c0)) OVER());
