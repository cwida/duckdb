SELECT NULL FROM (
	SELECT
		hex(CAST(CAST(NULL AS BLOB) AS BLOB)) AS c7,
		ref_0.r_comment AS c11
	FROM main.region AS ref_0
) AS subq_0
WHERE (
	(subq_0.c11 ~~ subq_0.c7)
);
