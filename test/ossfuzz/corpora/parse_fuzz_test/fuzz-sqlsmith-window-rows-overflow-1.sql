SELECT nth_value(1929, 42) OVER (
	PARTITION BY (
		"usmallint" > "smallint"
	), 747 ROWS BETWEEN GREATEST(COALESCE("bigint", 0), 0) PRECEDING AND CURRENT ROW
)
FROM all_types;
