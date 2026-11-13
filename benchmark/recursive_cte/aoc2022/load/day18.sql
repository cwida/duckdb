CREATE TABLE aoc2022_day18_input(x INTEGER, y INTEGER, z INTEGER);

INSERT INTO aoc2022_day18_input
SELECT x::INTEGER, y::INTEGER, z::INTEGER
FROM range(18) xs(x), range(18) ys(y), range(18) zs(z)
WHERE x IN (0, 17) OR y IN (0, 17) OR z IN (0, 17);
