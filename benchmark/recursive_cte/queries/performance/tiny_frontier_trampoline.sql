-- Trampoline formulation of tiny_frontier_union_all.sql: BRANCH TEE emits each
-- arrival once instead of accumulating through UNION ALL
-- ("Trampoline-Style Queries for SQL", CIDR 2025).
WITH RECURSIVE t(b, i) USING TRAMPOLINE (b) AS (
	(SELECT 1, 0::BIGINT)
	BRANCH 1 TEE: (SELECT CASE WHEN i + 1 < 49999 THEN 1 ELSE 0 END, i + 1 FROM t)
)
SELECT count(*) AS row_count, sum(i)::BIGINT AS value_sum, max(i) AS max_value
FROM t;
