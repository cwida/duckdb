SELECT nth_value(1929, 42) OVER (
	PARTITION BY (
		"usmallint" > "smallint"
	), 747 ROWS BETWEEN 1 PRECEDING AND GREATEST(COALESCE("bigint", 0), 0) PRECEDING
)
FROM all_types;
