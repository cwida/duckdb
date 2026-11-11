CREATE TEMP TABLE ans AS
SELECT id1, id2, id3, id4, id5, id6, sum(v3) AS v3, count(*) AS count
FROM (FROM x_group LIMIT 2800000)
GROUP BY id1, id2, id3, id4, id5, id6;
