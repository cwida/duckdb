-- Batch of gcd(x, y) argument pairs, deterministic.
-- Shape from "Trampoline-Style Queries for SQL" (CIDR'25), Figure 2.
CREATE TABLE args AS
SELECT i AS id,
       (hash(i) % 100000 + 1)::BIGINT AS x,
       (hash(i * 7 + 1) % 100000 + 1)::BIGINT AS y
FROM range(500000) t(i);
