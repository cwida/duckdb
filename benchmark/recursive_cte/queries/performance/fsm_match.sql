-- FSM simulation via state-dispatch recursive CTE: one row per candidate
-- match, states 1 (descending), 2 (bottom seen, ascending), 0 (accepted).
-- Pattern: START DOWN* BOTTOM UP* , reported as (start_seq, end_seq).
WITH RECURSIVE fsm(state, start_seq, seq) AS (
  SELECT 1, s.seq, s.seq
  FROM series s
  WHERE s.tag = 'START'
  UNION ALL
  SELECT CASE fsm.state
           WHEN 1 THEN CASE g.tag WHEN 'DOWN' THEN 1 WHEN 'BOTTOM' THEN 2 END
           WHEN 2 THEN CASE g.tag WHEN 'UP' THEN 2 ELSE 0 END
         END,
         fsm.start_seq,
         CASE WHEN fsm.state = 2 AND g.tag <> 'UP' THEN fsm.seq ELSE g.seq END
  FROM fsm
  JOIN series g ON g.seq = fsm.seq + 1
  WHERE fsm.state <> 0
    AND (fsm.state <> 1 OR g.tag IN ('DOWN', 'BOTTOM'))
)
SELECT count(*) AS matches, sum(seq - start_seq) AS total_len, max(seq - start_seq) AS max_len
FROM fsm
WHERE state = 0;
