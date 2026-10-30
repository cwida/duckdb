-- VM interpretation via a recursive CTE whose member is a UNION ALL of
-- per-location arms (one filtered scan of the working table per location),
-- mirroring the trampoline branch bodies without new syntax. b = 0 halts.
WITH RECURSIVE vm(b, r1, r2, r3, r4, r5, r6, r7) AS (
  SELECT 1, n::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT FROM inputs
  UNION ALL
  (
    SELECT 2,  r1, r2, r3, 0,  r5, r6, r7 FROM vm WHERE b = 1                        -- 0: lod r4 0
    UNION ALL
    SELECT 3,  r1, r2, r3, r4, 1,  r6, r7 FROM vm WHERE b = 2                        -- 1: lod r5 1
    UNION ALL
    SELECT 4,  r1, r2, r3, r4, r5, 2,  r7 FROM vm WHERE b = 3                        -- 2: lod r6 2
    UNION ALL
    SELECT 5,  r1, r2, r3, r4, r5, r6, 3  FROM vm WHERE b = 4                        -- 3: lod r7 3
    UNION ALL
    SELECT 6,  r1, r4, r3, r4, r5, r6, r7 FROM vm WHERE b = 5                        -- 4: mov r2 r4
    UNION ALL
    SELECT CASE WHEN r1 = r5 THEN 15 ELSE 7 END,
           r1, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 6                            -- 5: jeq r1 r5 14
    UNION ALL
    SELECT 8,  r1, r2 + r5, r3, r4, r5, r6, r7 FROM vm WHERE b = 7                   -- 6: add r2 r2 r5
    UNION ALL
    SELECT 9,  r1, r2, r1 % r6, r4, r5, r6, r7 FROM vm WHERE b = 8                   -- 7: mod r3 r1 r6
    UNION ALL
    SELECT CASE WHEN r3 = r5 THEN 12 ELSE 10 END,
           r1, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 9                            -- 8: jeq r3 r5 11
    UNION ALL
    SELECT 11, r1 // r6, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 10                 -- 9: div r1 r1 r6
    UNION ALL
    SELECT 6,  r1, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 11                       -- 10: jmp 5
    UNION ALL
    SELECT 13, r1 * r7, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 12                  -- 11: mul r1 r1 r7
    UNION ALL
    SELECT 14, r1 + r5, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 13                  -- 12: add r1 r1 r5
    UNION ALL
    SELECT 6,  r1, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 14                       -- 13: jmp 5
    UNION ALL
    SELECT 0,  r1, r2, r3, r4, r5, r6, r7 FROM vm WHERE b = 15                       -- 14: hlt r2
  )
)
SELECT count(*) AS halted, sum(r2) AS result_sum, max(r2) AS result_max
FROM vm
WHERE b = 0;
