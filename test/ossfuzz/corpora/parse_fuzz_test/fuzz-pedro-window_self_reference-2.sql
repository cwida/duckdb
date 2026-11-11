SELECT count(*) OVER w1 WINDOW w1 AS (ORDER BY count(*) OVER w2), w2 AS (ORDER BY count(*) OVER w1);
