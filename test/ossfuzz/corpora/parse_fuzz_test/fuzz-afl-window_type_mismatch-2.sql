SELECT rank() OVER (), lag(NULL) OVER (ORDER BY x) FROM (VALUES (1)) tbl(x);
