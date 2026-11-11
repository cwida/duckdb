SELECT count(*) OVER w1 WINDOW w1 AS (ORDER BY count(*) OVER w1);
