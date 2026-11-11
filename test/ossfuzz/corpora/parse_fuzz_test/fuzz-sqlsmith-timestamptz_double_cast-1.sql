SELECT 
	arg_min(CAST(ref_0.timestamp_tz AS TIMESTAMP WITH TIME ZONE), 
	CAST(make_timestamptz(
		CAST(ref_0."bigint" AS BIGINT), 
		CAST(ref_0."bigint" AS BIGINT), 
		CAST(ref_0."bigint" AS BIGINT), 
		CAST(ref_0."bigint" AS BIGINT), 
		CAST(txid_current() AS BIGINT), 
		CAST(ref_0."bigint" AS BIGINT)) AS TIMESTAMP WITH TIME ZONE)) 
		OVER (PARTITION BY ref_0."smallint" ORDER BY ref_0.int_array) AS c2 
FROM main.all_types AS ref_0;
