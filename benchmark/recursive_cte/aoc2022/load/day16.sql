CREATE TABLE aoc2022_day16_input(valve VARCHAR, flow INTEGER, tunnel VARCHAR);

INSERT INTO aoc2022_day16_input
WITH valves(id, valve, flow) AS (
	SELECT id,
	       'A' || chr(ascii('A') + id::INTEGER) AS valve,
	       CASE WHEN id = 0 THEN 0 ELSE ((id * 7) % 19 + 1)::INTEGER END AS flow
	FROM range(17) valves(id)
)
SELECT source.valve,
       source.flow,
       target.valve
FROM valves source,
     (VALUES (1), (3), (7)) offsets(delta),
     valves target
WHERE target.id = (source.id + offsets.delta) % 17;
