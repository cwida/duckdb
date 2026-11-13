WITH RECURSIVE
params(node_count, iterations) AS (
	VALUES (12000, 30)
),
nodes(node) AS (
	SELECT node::INTEGER FROM params, range(node_count) nodes(node)
),
edges(here, there) AS (
	SELECT DISTINCT node, CASE WHEN edge_nr = 0 THEN (node + 1) % node_count
		ELSE (node + edge_nr * edge_nr + (node % 97) * edge_nr) % node_count END
	FROM params, nodes, range(8) edges(edge_nr)
),
edge_degrees(here, there, outgoing) AS (
	SELECT here, there, count(*) OVER (PARTITION BY here) FROM edges
),
pagerank(node, rank, iter) USING KEY (node) AS (
	SELECT node, 1.0 / node_count, 0 FROM nodes, params
	UNION ALL
	SELECT contribution.node,
		((1 - 0.9) / node_count) + 0.9 * sum(contribution.rank), contribution.iter + 1
	FROM (
		SELECT edge.there AS node, current.rank / edge.outgoing AS rank, current.iter
		FROM recurring.pagerank current
		JOIN edge_degrees edge ON current.node = edge.here
	) contribution, params
	WHERE contribution.iter < iterations
	GROUP BY contribution.node, contribution.iter, node_count
)
SELECT count(*) AS nodes, max(iter) AS iterations,
	round(sum(rank), 12) AS rank_sum,
	round(sum(node * rank), 8) AS rank_checksum
FROM pagerank;
