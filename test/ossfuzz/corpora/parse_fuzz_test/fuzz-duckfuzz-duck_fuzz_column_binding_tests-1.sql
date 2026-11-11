SELECT     ref_8.uint AS c0,
           Argmin(Cast(ref_8."timestamp" AS TIMESTAMP), Cast(ref_10."timestamp" AS TIMESTAMP)) OVER (partition BY ref_10."nested_int_array", ref_10."timestamp" ORDER BY ref_8.hugeint)  AS c1
FROM main.all_types AS ref_8
INNER JOIN main.all_types AS ref_9
INNER JOIN main.all_types AS ref_10
ON        ( ref_10.dec38_10 IS NOT NULL)
                      OR         EXISTS
                                 ( SELECT ref_9."float" FROM main.all_types)
ON         ((ref_9."smallint" = ref_8."smallint"));
