SELECT subtract(
	CAST(main.all_types."interval" AS INTERVAL), 
	CAST(to_microseconds(CAST(main.all_types."bigint" AS BIGINT)) AS INTERVAL))
FROM main.all_types;
