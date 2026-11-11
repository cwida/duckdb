SELECT arg_min(CAST(ref_5."varchar" AS VARCHAR), CAST(ref_4."varchar" AS VARCHAR)) OVER (PARTITION BY ref_5."bit") 
FROM main.all_types AS ref_4 
INNER JOIN main.all_types AS ref_5 ON (ref_5.hugeint IS NOT NULL)
INNER JOIN main.all_types AS ref_7 ON (ref_4."int" = ref_5."int");
