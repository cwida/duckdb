-- 50k one-row iterations with the counter split into two parity arms, to
-- expose the per-arm working-table scan cost at one-row frontiers. The rcte
-- formulation is the single-arm form of the same loop.
WITH RECURSIVE t(i) AS (
	VALUES (0::BIGINT)
	UNION ALL
	(
		SELECT i + 1 FROM t WHERE i % 2 = 0 AND i < 49999
		UNION ALL
		SELECT i + 1 FROM t WHERE i % 2 = 1 AND i < 49999
	)
)
SELECT count(*) AS row_count, sum(i)::BIGINT AS value_sum, max(i) AS max_value
FROM t;
