-- gcd via a recursive CTE whose member is a UNION ALL of per-pc arms
-- (one filtered scan of the working table per pc), mirroring the trampoline
-- branches without new syntax. pc = 0 halts.
WITH RECURSIVE gcd(pc, id, a, b, t) AS (
  SELECT 1, id, x, y, NULL::BIGINT FROM args
  UNION ALL
  (
    SELECT CASE WHEN b <> 0 THEN 2 ELSE 0 END, id, a, b, t FROM gcd WHERE pc = 1
    UNION ALL
    SELECT 3, id, a, a % b, b FROM gcd WHERE pc = 2
    UNION ALL
    SELECT 1, id, t, b, t FROM gcd WHERE pc = 3
  )
)
SELECT count(*) AS results, sum(a) AS gcd_sum, max(a) AS gcd_max
FROM gcd
WHERE pc = 0;
