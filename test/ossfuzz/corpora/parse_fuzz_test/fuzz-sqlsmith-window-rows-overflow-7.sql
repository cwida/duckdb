SELECT nth_value(1929, 42) OVER (
	PARTITION BY (
		"usmallint" > "smallint"
	), 747 ROWS BETWEEN CURRENT ROW AND GREATEST(COALESCE("bigint", 0), 0) FOLLOWING
)
FROM all_types;
