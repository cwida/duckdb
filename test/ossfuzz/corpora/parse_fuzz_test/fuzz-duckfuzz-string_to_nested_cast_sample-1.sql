SELECT COUNT(*) FILTER (cast_part IS NULL)
FROM (
	SELECT TRY_CAST(p_name AS STRUCT(a INTEGER[], b VARCHAR[])) FROM part USING SAMPLE 81.0% (Reservoir)
) t(cast_part);
