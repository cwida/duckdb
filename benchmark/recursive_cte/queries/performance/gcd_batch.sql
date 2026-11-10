-- gcd via pc-dispatch recursive CTE, 3 branches, faithful to CIDR'25 Fig. 2:
--   pc 1: loop test        b <> 0 ? goto 2 : halt (pc 0)
--   pc 2: t := b; b := a % b; goto 3
--   pc 3: a := t; goto 1
WITH RECURSIVE gcd(pc, id, a, b, t) AS (
  SELECT 1, id, x, y, NULL::BIGINT FROM args
  UNION ALL
  SELECT CASE pc WHEN 1 THEN CASE WHEN b <> 0 THEN 2 ELSE 0 END
                 WHEN 2 THEN 3
                 WHEN 3 THEN 1
         END,
         id,
         CASE pc WHEN 3 THEN t ELSE a END,
         CASE pc WHEN 2 THEN a % b ELSE b END,
         CASE pc WHEN 2 THEN b ELSE t END
  FROM gcd
  WHERE pc <> 0
)
SELECT count(*) AS results, sum(a) AS gcd_sum, max(a) AS gcd_max
FROM gcd
WHERE pc = 0;
