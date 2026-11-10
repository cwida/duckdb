-- Trampoline formulation of gcd_batch.sql: selector-routed recursion in the style
-- of "Trampoline-Style Queries for SQL" (CIDR 2025).
-- gcd via WITH TRAMPOLINE, 3 branches, faithful to CIDR'25 Fig. 2:
--   branch 1: loop test   b <> 0 ? goto 2 : halt (emit)
--   branch 2: t := b; b := a % b; goto 3
--   branch 3: a := t; goto 1
WITH RECURSIVE gcd(pc, id, a, b, t) USING TRAMPOLINE (pc) AS (
  (SELECT 1, id, x, y, NULL::BIGINT FROM args)
  BRANCH 1: (SELECT CASE WHEN b <> 0 THEN 2 ELSE 0 END, id, a, b, t FROM gcd)
  BRANCH 2: (SELECT 3, id, a, a % b, b FROM gcd)
  BRANCH 3: (SELECT 1, id, t, b, t FROM gcd)
)
SELECT count(*) AS results, sum(a) AS gcd_sum, max(a) AS gcd_max
FROM gcd;
