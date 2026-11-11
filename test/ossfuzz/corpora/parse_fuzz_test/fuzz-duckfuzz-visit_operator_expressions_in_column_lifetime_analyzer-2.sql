SELECT count_star() 
OVER (PARTITION BY ref_1.hugeint, ref_1."timestamp" ORDER BY subq_0.c0) AS c0 
FROM 
(SELECT 
ref_0."int" AS c0, 
ref_0.timestamp_s AS c1 
FROM main.all_types AS ref_0 
WHERE 
ref_0."varchar") AS subq_0 
INNER JOIN
main.all_types AS ref_1 
ON 
((subq_0.c1 = ref_1.timestamp_s))
WHERE (ref_1."time" IS NOT NULL);
