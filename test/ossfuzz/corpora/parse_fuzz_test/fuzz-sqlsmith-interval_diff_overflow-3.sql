SELECT subtract(
	CAST(main.all_types."interval" AS INTERVAL), 
	CAST(to_days(CAST(main.all_types."int" AS INTEGER)) AS INTERVAL))
FROM main.all_types;
