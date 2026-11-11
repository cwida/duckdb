SELECT SUM(i), SUM(i) FILTER (i%2) FROM range(10) tbl(i);
