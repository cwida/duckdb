WITH RECURSIVE
params(width, height, iterations) AS (
	VALUES (21, 21, 70)
),
initial(x, y, val) AS (
	SELECT x, y, (((x * 17 + y * 31 + x * y * 7) % 11) < 5)::INTEGER
	FROM params, range(21) xs(x), range(21) ys(y)
),
game_of_life(iter, x, y, val) USING KEY (x, y) AS (
	SELECT 0, x, y, val FROM initial
	UNION ALL
	SELECT cell.iter + 1, cell.x, cell.y,
		CASE sum(neighbor.val) WHEN 2 THEN cell.val WHEN 3 THEN 1 ELSE 0 END
	FROM recurring.game_of_life cell, recurring.game_of_life neighbor, params
	WHERE (cell.x - neighbor.x) % width BETWEEN -1 AND 1
	  AND (cell.y - neighbor.y) % height BETWEEN -1 AND 1
	  AND (cell.x != neighbor.x OR cell.y != neighbor.y)
	  AND cell.iter < iterations
	GROUP BY cell.iter, cell.x, cell.y, cell.val
)
SELECT max(iter) AS iterations, count(*) AS cells, sum(val)::BIGINT AS live_cells
FROM game_of_life;
