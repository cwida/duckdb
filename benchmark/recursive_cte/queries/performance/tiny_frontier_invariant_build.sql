WITH RECURSIVE bidirectional_edges AS (
    SELECT src, dst FROM edges
    UNION ALL
    SELECT dst AS src, src AS dst
    FROM edges
    WHERE src <> dst
), walk(node, depth) AS (
    SELECT 65504::BIGINT, 0
    UNION ALL
    SELECT e.dst, w.depth + 1
    FROM walk w
    JOIN bidirectional_edges e ON e.src = w.node
    WHERE w.depth < 2
)
SELECT depth, count(*) AS paths, sum(node) AS checksum
FROM walk
GROUP BY depth
ORDER BY depth;
