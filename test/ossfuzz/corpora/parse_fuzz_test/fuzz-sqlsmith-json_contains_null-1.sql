WITH cte AS (
    SELECT NULL::VARCHAR j
    FROM range(1)
)
SELECT json_contains(j, j)
FROM cte;
