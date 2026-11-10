-- FSM simulation via a recursive CTE whose member is a UNION ALL of per-state
-- arms (one filtered scan of the working table per state), mirroring the
-- trampoline branches without new syntax. state = 0 accepts.
WITH RECURSIVE fsm(state, start_seq, seq) AS (
  SELECT 1, s.seq, s.seq
  FROM series s
  WHERE s.tag = 'START'
  UNION ALL
  (
    SELECT CASE g.tag WHEN 'DOWN' THEN 1 WHEN 'BOTTOM' THEN 2 END,
           fsm.start_seq, g.seq
    FROM fsm
    JOIN series g ON g.seq = fsm.seq + 1
    WHERE fsm.state = 1 AND g.tag IN ('DOWN', 'BOTTOM')
    UNION ALL
    SELECT CASE WHEN g.tag = 'UP' THEN 2 ELSE 0 END,
           fsm.start_seq,
           CASE WHEN g.tag = 'UP' THEN g.seq ELSE fsm.seq END
    FROM fsm
    JOIN series g ON g.seq = fsm.seq + 1
    WHERE fsm.state = 2
  )
)
SELECT count(*) AS matches, sum(seq - start_seq) AS total_len, max(seq - start_seq) AS max_len
FROM fsm
WHERE state = 0;
