SELECT max(CAST(from_hex(CAST(ref_0.r_name AS VARCHAR)) AS BLOB)) OVER (ORDER BY ref_0.r_name) AS c5,
FROM tbl AS ref_0;
