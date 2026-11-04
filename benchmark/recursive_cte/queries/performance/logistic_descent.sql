-- Recursive-CTE formulation: selector column routes between the loop (1) and the exit (0).
WITH RECURSIVE m(b, w, i) AS (
	(SELECT 1, 0.0::DOUBLE, 0::BIGINT)
	UNION ALL
	(SELECT CASE WHEN i + 1 >= 5000 THEN 0 ELSE 1 END,
	        w - 0.5 * (SELECT avg(((1.0 / (1.0 + exp(-w * f))) - y) * f) FROM obs),
	        i + 1
	 FROM m WHERE b = 1)
)
SELECT i, round(w, 6) AS w FROM m WHERE b = 0;
