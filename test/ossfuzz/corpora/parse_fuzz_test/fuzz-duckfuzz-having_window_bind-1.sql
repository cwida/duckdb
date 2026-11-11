SELECT
	DISTINCT c7,
	(c3 < NULL),
	c9
FROM
	(
		SELECT
			DISTINCT c8,
			c2,
			rank() OVER (
				ORDER BY
					c6 RANGE BETWEEN CURRENT ROW
					AND c2 FOLLOWING
			)
		FROM
			nation AS t5(c1, c2, c3, c4) ASOF
			INNER JOIN (
				SELECT
					DISTINCT c1
				FROM
					nation AS t10(c6, c7, c8, c9)
				HAVING
					nth_value(
						c8,
						#8) OVER (
							RANGE BETWEEN '24:00:00-15:59:59'::TIME WITH TIME ZONE PRECEDING 
							  AND 'c7607984-ed9f-4f50-a6e3-62b94b3a6e9c' PRECEDING) 
				USING SAMPLE 19% (System) 
				ORDER BY * DESC NULLS FIRST
			) AS t11 ON (8252) 
		WHERE 'enum_299' 
		HAVING c2 
		ORDER BY *
	) AS t12 
ORDER BY * ASC;
