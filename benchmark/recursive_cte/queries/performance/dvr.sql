WITH RECURSIVE
params(node_count) AS (
	VALUES (150)
),
edges(person1id, person2id) AS (
	SELECT node::BIGINT, ((node + edge_offset) % node_count)::BIGINT
	FROM params, range(node_count) nodes(node), (VALUES (1), (2), (5), (13)) offsets(edge_offset)
),
dvr(here, there, via, cost) USING KEY (here, there) AS (
	SELECT person1id, person2id, person2id, 1::DOUBLE FROM edges
	UNION ALL
	(
		SELECT edge.person1id, dvr.there, dvr.here, 1 + dvr.cost
		FROM dvr
		JOIN edges edge ON edge.person2id = dvr.here AND edge.person1id != dvr.there
		LEFT JOIN recurring.dvr rec
		  ON rec.here = edge.person1id AND rec.there = dvr.there
		WHERE 1 + dvr.cost < coalesce(rec.cost, 'Infinity'::DOUBLE)
		ORDER BY 1 + dvr.cost
	)
)
SELECT count(*) AS routes, sum(cost)::BIGINT AS total_cost,
	max(cost)::BIGINT AS maximum_cost
FROM dvr;
