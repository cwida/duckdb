-- Trampoline formulation of asql_marching_squares.sql: DISTINCT admission
-- replaces the UNION-recursion deduplication, TEE emits each admitted cell
-- ("Trampoline-Style Queries for SQL", CIDR 2025).
WITH RECURSIVE
pixels(x, y, filled) AS (
	SELECT x, y, altitude >= 200
	FROM asql_marching_map
),
squares(x, y, lower_left, lower_right, upper_left, upper_right) AS MATERIALIZED (
	SELECT lower_left.x,
	       lower_left.y,
	       lower_left.filled,
	       lower_right.filled,
	       upper_left.filled,
	       upper_right.filled
	FROM pixels AS lower_left,
	     pixels AS lower_right,
	     pixels AS upper_left,
	     pixels AS upper_right
	WHERE (lower_right.x, lower_right.y) = (lower_left.x + 1, lower_left.y)
	  AND (upper_left.x, upper_left.y) = (lower_left.x, lower_left.y + 1)
	  AND (upper_right.x, upper_right.y) = (lower_left.x + 1, lower_left.y + 1)
),
march(b, x, y) USING TRAMPOLINE (b) EMIT DISTINCT AS (
	(SELECT 1, min_x - 1, min_y - 1 FROM asql_marching_config)
	BRANCH 1 DISTINCT TEE: (
		SELECT 1,
		       march.x + directions.delta_x,
		       march.y + directions.delta_y
		FROM march, squares, asql_marching_directions AS directions
		WHERE (march.x, march.y) = (squares.x, squares.y)
		  AND (
			  squares.lower_left,
			  squares.lower_right,
			  squares.upper_left,
			  squares.upper_right
		  ) = (
			  directions.lower_left,
			  directions.lower_right,
			  directions.upper_left,
			  directions.upper_right
		  )
	)
)
SELECT count(*)::BIGINT AS points,
       min(x)::INTEGER AS min_x,
       max(x)::INTEGER AS max_x,
       min(y)::INTEGER AS min_y,
       max(y)::INTEGER AS max_y,
       md5(string_agg(format('{}:{}', x, y), ',' ORDER BY x, y)) AS checksum
FROM march;
