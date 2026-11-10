-- VM interpretation via pc-dispatch recursive CTE: each row is one VM run,
-- dispatch on the opcode at the current location (9 branches).
-- vA/vB/vC are the current values of the registers named by reg1/reg2/reg3.
WITH RECURSIVE vm(n, loc, opc, reg1, reg2, reg3, r1, r2, r3, r4, r5, r6, r7) AS (
  SELECT i.n, p.loc, p.opc, p.reg1, p.reg2, p.reg3,
         i.n::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT, 0::BIGINT
  FROM inputs i, program p
  WHERE p.loc = 0
  UNION ALL
  SELECT s.n, p.loc, p.opc, p.reg1, p.reg2, p.reg3,
         CASE WHEN s.wr AND s.reg1 = 1 THEN s.val ELSE s.r1 END,
         CASE WHEN s.wr AND s.reg1 = 2 THEN s.val ELSE s.r2 END,
         CASE WHEN s.wr AND s.reg1 = 3 THEN s.val ELSE s.r3 END,
         CASE WHEN s.wr AND s.reg1 = 4 THEN s.val ELSE s.r4 END,
         CASE WHEN s.wr AND s.reg1 = 5 THEN s.val ELSE s.r5 END,
         CASE WHEN s.wr AND s.reg1 = 6 THEN s.val ELSE s.r6 END,
         CASE WHEN s.wr AND s.reg1 = 7 THEN s.val ELSE s.r7 END
  FROM (
    SELECT *,
           opc IN ('lod', 'mov', 'add', 'mul', 'div', 'mod') AS wr,
           CASE opc
             WHEN 'lod' THEN reg2::BIGINT
             WHEN 'mov' THEN vB
             WHEN 'add' THEN vB + vC
             WHEN 'mul' THEN vB * vC
             WHEN 'div' THEN vB // vC
             WHEN 'mod' THEN vB % vC
           END AS val,
           CASE opc
             WHEN 'jeq' THEN CASE WHEN vA = vB THEN reg3 ELSE loc + 1 END
             WHEN 'jmp' THEN reg1
             ELSE loc + 1
           END AS next_loc
    FROM (
      SELECT *,
             CASE reg1 WHEN 1 THEN r1 WHEN 2 THEN r2 WHEN 3 THEN r3 WHEN 4 THEN r4
                       WHEN 5 THEN r5 WHEN 6 THEN r6 WHEN 7 THEN r7 END AS vA,
             CASE reg2 WHEN 1 THEN r1 WHEN 2 THEN r2 WHEN 3 THEN r3 WHEN 4 THEN r4
                       WHEN 5 THEN r5 WHEN 6 THEN r6 WHEN 7 THEN r7 END AS vB,
             CASE reg3 WHEN 1 THEN r1 WHEN 2 THEN r2 WHEN 3 THEN r3 WHEN 4 THEN r4
                       WHEN 5 THEN r5 WHEN 6 THEN r6 WHEN 7 THEN r7 END AS vC
      FROM vm
      WHERE opc <> 'hlt'
    )
  ) s
  JOIN program p ON p.loc = s.next_loc
)
SELECT count(*) AS halted,
       sum(CASE reg1 WHEN 1 THEN r1 WHEN 2 THEN r2 WHEN 3 THEN r3 END) AS result_sum,
       max(CASE reg1 WHEN 1 THEN r1 WHEN 2 THEN r2 WHEN 3 THEN r3 END) AS result_max
FROM vm
WHERE opc = 'hlt';
