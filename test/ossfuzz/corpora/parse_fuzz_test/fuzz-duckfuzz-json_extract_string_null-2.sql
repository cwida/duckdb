SELECT json_extract_string(false, COALESCE(CASE WHEN (i) THEN (CAST(NULL AS "json")) ELSE NULL END, NULL)) FROM integers;
