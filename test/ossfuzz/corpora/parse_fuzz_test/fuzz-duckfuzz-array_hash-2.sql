SELECT subq_0.c2 AS c4
FROM (
	SELECT ref_1.fixed_nested_int_array AS c2
	FROM main.all_types AS ref_0
	LEFT JOIN main.all_types
		AS ref_1
		ON ((ref_0."varchar" !~~* ref_1."varchar"))
	) AS subq_0
RIGHT JOIN main.all_types
	AS ref_2
	ON ((subq_0.c2 IS NOT DISTINCT FROM ref_2.fixed_nested_int_array));
