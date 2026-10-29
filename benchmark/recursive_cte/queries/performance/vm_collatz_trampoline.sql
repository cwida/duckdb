-- Trampoline formulation of vm_collatz.sql: selector-routed recursion in the style
-- of "Trampoline-Style Queries for SQL" (CIDR 2025).
-- VM interpretation via WITH TRAMPOLINE: one branch per program location
-- (loc L = branch L+1), register updates hard-coded per instruction, the
-- shape a PL-to-SQL compiler emits. hlt (loc 14) emits r2.
WITH RECURSIVE vm(b, r1, r2, r3, r4, r5, r6, r7) USING TRAMPOLINE (b) AS (
  (SELECT 1, n::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT FROM inputs)
  BRANCH 1:  (SELECT 2,  r1, r2, r3, 0,  r5, r6, r7 FROM vm)                       -- 0: lod r4 0
  BRANCH 2:  (SELECT 3,  r1, r2, r3, r4, 1,  r6, r7 FROM vm)                       -- 1: lod r5 1
  BRANCH 3:  (SELECT 4,  r1, r2, r3, r4, r5, 2,  r7 FROM vm)                       -- 2: lod r6 2
  BRANCH 4:  (SELECT 5,  r1, r2, r3, r4, r5, r6, 3  FROM vm)                       -- 3: lod r7 3
  BRANCH 5:  (SELECT 6,  r1, r4, r3, r4, r5, r6, r7 FROM vm)                       -- 4: mov r2 r4
  BRANCH 6:  (SELECT CASE WHEN r1 = r5 THEN 15 ELSE 7 END,
                     r1, r2, r3, r4, r5, r6, r7 FROM vm)                           -- 5: jeq r1 r5 14
  BRANCH 7:  (SELECT 8,  r1, r2 + r5, r3, r4, r5, r6, r7 FROM vm)                  -- 6: add r2 r2 r5
  BRANCH 8:  (SELECT 9,  r1, r2, r1 % r6, r4, r5, r6, r7 FROM vm)                  -- 7: mod r3 r1 r6
  BRANCH 9:  (SELECT CASE WHEN r3 = r5 THEN 12 ELSE 10 END,
                     r1, r2, r3, r4, r5, r6, r7 FROM vm)                           -- 8: jeq r3 r5 11
  BRANCH 10: (SELECT 11, r1 // r6, r2, r3, r4, r5, r6, r7 FROM vm)                 -- 9: div r1 r1 r6
  BRANCH 11: (SELECT 6,  r1, r2, r3, r4, r5, r6, r7 FROM vm)                       -- 10: jmp 5
  BRANCH 12: (SELECT 13, r1 * r7, r2, r3, r4, r5, r6, r7 FROM vm)                  -- 11: mul r1 r1 r7
  BRANCH 13: (SELECT 14, r1 + r5, r2, r3, r4, r5, r6, r7 FROM vm)                  -- 12: add r1 r1 r5
  BRANCH 14: (SELECT 6,  r1, r2, r3, r4, r5, r6, r7 FROM vm)                       -- 13: jmp 5
  BRANCH 15: (SELECT 0,  r1, r2, r3, r4, r5, r6, r7 FROM vm)                       -- 14: hlt r2
)
SELECT count(*) AS halted, sum(r2) AS result_sum, max(r2) AS result_max
FROM vm;
