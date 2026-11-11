SELECT nth_value(1929, 42) OVER (
	PARTITION BY (
		"usmallint" > "smallint"
	), 747 ROWS BETWEEN GREATEST(COALESCE("bigint", 0), 0) FOLLOWING AND 1 FOLLOWING
)
FROM all_types;
