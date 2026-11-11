SELECT
	NULL
FROM
main.partsupp AS ref_1
INNER JOIN main.region ON (NULL)
INNER JOIN main.customer AS ref_3 ON
(EXISTS(SELECT NULL FROM (SELECT ref_3.c_acctbal AS c0)))
INNER JOIN
(SELECT
	ref_9.s_phone AS c0,
	ref_8.r_regionkey AS c1,
	ref_9.s_phone AS c2
FROM main.region AS ref_8
RIGHT JOIN
	main.supplier AS ref_9 ON ((1 AND (ref_9.s_comment ~~ ref_9.s_address) AND 0 AND (ref_9.s_name !~~ ref_9.s_name)))
LEFT JOIN main.customer AS ref_10 ON ((ref_10.c_address ~~* ref_10.c_comment))
WHERE (ref_9.s_phone IS NOT NULL) LIMIT 53) AS subq_2
ON ((CASE  WHEN (1) THEN (subq_2.c2) ELSE subq_2.c2 END !~~ ref_1.ps_comment));
