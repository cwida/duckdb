-- Trampoline formulation of fsm_match.sql: selector-routed recursion in the style
-- of "Trampoline-Style Queries for SQL" (CIDR 2025).
-- FSM simulation via WITH TRAMPOLINE: one row per candidate match,
-- branch 1 = descending, branch 2 = bottom seen / ascending, accept emits.
-- Pattern: START DOWN* BOTTOM UP* , reported as (start_seq, end_seq).
WITH RECURSIVE fsm(state, start_seq, seq) USING TRAMPOLINE (state) AS (
  (SELECT 1, s.seq, s.seq FROM series s WHERE s.tag = 'START')
  BRANCH 1: (SELECT CASE g.tag WHEN 'DOWN' THEN 1 WHEN 'BOTTOM' THEN 2 END,
                    fsm.start_seq, g.seq
             FROM fsm
             JOIN series g ON g.seq = fsm.seq + 1
             WHERE g.tag IN ('DOWN', 'BOTTOM'))
  BRANCH 2: (SELECT CASE WHEN g.tag = 'UP' THEN 2 ELSE 0 END,
                    fsm.start_seq,
                    CASE WHEN g.tag = 'UP' THEN g.seq ELSE fsm.seq END
             FROM fsm
             JOIN series g ON g.seq = fsm.seq + 1)
)
SELECT count(*) AS matches, sum(seq - start_seq) AS total_len, max(seq - start_seq) AS max_len
FROM fsm;
