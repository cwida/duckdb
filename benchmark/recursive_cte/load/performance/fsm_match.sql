-- Ordered, tagged event series for FSM-style row pattern matching,
-- after the MATCH_RECOGNIZE use case in "Trampoline-Style Queries for SQL"
-- (CIDR'25), Section 3.1. Deterministic synthetic data: a hash-driven
-- price walk tagged START / DOWN / BOTTOM / UP by local shape.
CREATE TABLE series AS
WITH prices AS (
  SELECT i AS seq,
         1000 + (hash(i // 16) % 200)::BIGINT
              + ((hash(i) % 100)::BIGINT - 50) AS price
  FROM range(2000000) t(i)
),
tagged AS (
  SELECT seq, price,
         lag(price) OVER (ORDER BY seq)  AS prev,
         lead(price) OVER (ORDER BY seq) AS next
  FROM prices
)
SELECT seq, price,
       CASE
         WHEN prev IS NULL OR next IS NULL      THEN 'OTHER'
         WHEN price <  prev AND price <= next   THEN 'BOTTOM'
         WHEN price <  prev                     THEN 'DOWN'
         WHEN price >  prev AND prev <= coalesce(lag(prev) OVER (ORDER BY seq), prev)
                                                THEN 'UP'
         WHEN price >= prev                     THEN 'START'
         ELSE 'OTHER'
       END AS tag
FROM tagged;
